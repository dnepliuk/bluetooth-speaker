#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "i2s_output.h"
#include "status_led.h"

static const char *TAG = APP_LOG_TAG;

typedef struct
{
    uint64_t callbacks;
    uint64_t received_bytes;
    uint64_t queued_bytes;
    uint64_t dropped_bytes;
    uint32_t first_packet_size;
    uint32_t last_packet_size;
    uint64_t stream_read_bytes;
    uint64_t i2s_written_bytes;
    uint64_t i2s_write_operations;
    uint64_t i2s_write_errors;
    uint64_t i2s_short_writes;
    uint64_t diagnostic_consumed_bytes;
    uint64_t buffer_underflows;
    uint64_t prefetch_resumes;
    uint64_t latency_trim_events;
    uint64_t latency_trimmed_bytes;
    uint64_t discarded_while_stopped_bytes;
    uint64_t pcm_gap_events;
    uint32_t last_pcm_gap_ms;
    uint32_t max_pcm_gap_ms;
} audio_stats_t;

static StreamBufferHandle_t audio_stream;
static volatile uint32_t requested_sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
static uint32_t applied_sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
static volatile bool audio_stream_active;
static volatile bool audio_writer_prefetching = true;
static volatile bool first_audio_packet_log_pending;
static volatile bool pcm_gap_reset_requested = true;
static TickType_t last_pcm_callback_tick;

static portMUX_TYPE audio_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_stats_t audio_stats;
static portMUX_TYPE rssi_lock = portMUX_INITIALIZER_UNLOCKED;
static int8_t bluetooth_rssi_delta;
static bool bluetooth_rssi_delta_valid;

static void audio_stats_snapshot(audio_stats_t *snapshot)
{
    portENTER_CRITICAL(&audio_stats_lock);
    *snapshot = audio_stats;
    portEXIT_CRITICAL(&audio_stats_lock);
}

void audio_pipeline_set_streaming(bool streaming)
{
    __atomic_store_n(&pcm_gap_reset_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&audio_stream_active, streaming, __ATOMIC_RELAXED);
}

void audio_pipeline_set_sample_rate(uint32_t sample_rate_hz)
{
    __atomic_store_n(
        &requested_sample_rate_hz,
        sample_rate_hz,
        __ATOMIC_RELAXED);
}

void audio_pipeline_update_rssi(int8_t rssi_delta, bool valid)
{
    portENTER_CRITICAL(&rssi_lock);
    bluetooth_rssi_delta = rssi_delta;
    bluetooth_rssi_delta_valid = valid;
    portEXIT_CRITICAL(&rssi_lock);
}

static void audio_writer_task(void *argument)
{
    (void)argument;

#if AUDIO_DIAGNOSTIC_DRAIN_PCM
    uint8_t drain_buffer[AUDIO_WRITE_BUFFER_SIZE];

    __atomic_store_n(
        &audio_writer_prefetching,
        false,
        __ATOMIC_RELAXED);
    ESP_LOGW(TAG,
             "Diagnostic audio drain active: decoded PCM will be consumed "
             "without calls to i2s_channel_write");

    while (true)
    {
        size_t received = xStreamBufferReceive(
            audio_stream,
            drain_buffer,
            sizeof(drain_buffer),
            portMAX_DELAY);
        if (received > 0)
        {
            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.stream_read_bytes += received;
            audio_stats.diagnostic_consumed_bytes += received;
            portEXIT_CRITICAL(&audio_stats_lock);
        }
    }
#else
    uint8_t write_buffer[AUDIO_WRITE_BUFFER_SIZE];
    TickType_t next_rate_retry_at = 0;
    TickType_t last_i2s_error_log_at = 0;
    TickType_t last_short_write_log_at = 0;
    TickType_t last_underflow_log_at = 0;
    TickType_t last_prefetch_log_at = 0;
    bool prefetching = true;
    bool was_streaming = false;

    while (true)
    {
        TickType_t loop_started_at = xTaskGetTickCount();
        uint32_t new_sample_rate_hz =
            __atomic_load_n(&requested_sample_rate_hz, __ATOMIC_RELAXED);
        if (new_sample_rate_hz != applied_sample_rate_hz &&
            (next_rate_retry_at == 0 ||
             (int32_t)(loop_started_at - next_rate_retry_at) >= 0))
        {
            esp_err_t err = i2s_output_set_sample_rate(new_sample_rate_hz);
            if (err != ESP_OK)
            {
                next_rate_retry_at =
                    loop_started_at + pdMS_TO_TICKS(1000);
                status_led_signal_error();
            }
            else
            {
                applied_sample_rate_hz = new_sample_rate_hz;
                next_rate_retry_at = 0;
            }
        }

        bool streaming =
            __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);
        if (streaming && !was_streaming)
        {
            prefetching = true;
            __atomic_store_n(
                &audio_writer_prefetching,
                true,
                __ATOMIC_RELAXED);
            ESP_LOGI(TAG,
                     "Audio prefetch started: waiting for %u/%u bytes",
                     (unsigned)AUDIO_PREFETCH_THRESHOLD,
                     (unsigned)AUDIO_STREAM_BUFFER_SIZE);
        }
        else if (!streaming && was_streaming)
        {
            prefetching = true;
            __atomic_store_n(
                &audio_writer_prefetching,
                true,
                __ATOMIC_RELAXED);
        }
        was_streaming = streaming;

        size_t buffered_before_read =
            xStreamBufferBytesAvailable(audio_stream);
        if (streaming && prefetching &&
            buffered_before_read >= AUDIO_PREFETCH_THRESHOLD)
        {
            prefetching = false;
            __atomic_store_n(
                &audio_writer_prefetching,
                false,
                __ATOMIC_RELAXED);

            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.prefetch_resumes++;
            uint64_t prefetch_resumes = audio_stats.prefetch_resumes;
            portEXIT_CRITICAL(&audio_stats_lock);

            TickType_t now = xTaskGetTickCount();
            if (prefetch_resumes == 1 ||
                (now - last_prefetch_log_at) >= pdMS_TO_TICKS(1000))
            {
                ESP_LOGI(TAG,
                         "Audio prefetch complete: buffered=%u bytes, "
                         "I2S playback resumed, resumes=%" PRIu64,
                         (unsigned)buffered_before_read,
                         prefetch_resumes);
                last_prefetch_log_at = now;
            }
        }

        if (streaming && !prefetching &&
            buffered_before_read > AUDIO_LATENCY_HIGH_WATER)
        {
            size_t bytes_to_trim =
                buffered_before_read - AUDIO_LATENCY_TARGET;
            bytes_to_trim -= bytes_to_trim % AUDIO_PCM_FRAME_BYTES;
            size_t trimmed = 0;

            while (trimmed < bytes_to_trim)
            {
                size_t remaining = bytes_to_trim - trimmed;
                size_t chunk = remaining < sizeof(write_buffer)
                                   ? remaining
                                   : sizeof(write_buffer);
                size_t discarded = xStreamBufferReceive(
                    audio_stream,
                    write_buffer,
                    chunk,
                    0);
                if (discarded == 0)
                {
                    break;
                }
                trimmed += discarded;
            }

            if (trimmed > 0)
            {
                portENTER_CRITICAL(&audio_stats_lock);
                audio_stats.stream_read_bytes += trimmed;
                audio_stats.latency_trim_events++;
                audio_stats.latency_trimmed_bytes += trimmed;
                portEXIT_CRITICAL(&audio_stats_lock);
            }
        }

        size_t received = 0;
        if (!streaming)
        {
            size_t discarded = xStreamBufferReceive(
                audio_stream,
                write_buffer,
                sizeof(write_buffer),
                0);
            if (discarded > 0)
            {
                portENTER_CRITICAL(&audio_stats_lock);
                audio_stats.discarded_while_stopped_bytes += discarded;
                portEXIT_CRITICAL(&audio_stats_lock);
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(AUDIO_PREFETCH_POLL_MS));
            }
        }
        else if (!prefetching)
        {
            received = xStreamBufferReceive(
                audio_stream,
                write_buffer,
                sizeof(write_buffer),
                pdMS_TO_TICKS(AUDIO_RECEIVE_TIMEOUT_MS));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_PREFETCH_POLL_MS));
        }

        if (received > 0)
        {
            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.stream_read_bytes += received;
            portEXIT_CRITICAL(&audio_stats_lock);

            size_t bytes_written = 0;
            esp_err_t err = i2s_output_write(
                write_buffer,
                received,
                &bytes_written);

            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.i2s_write_operations++;
            audio_stats.i2s_written_bytes += bytes_written;
            if (err != ESP_OK)
            {
                audio_stats.i2s_write_errors++;
            }
            if (bytes_written != received)
            {
                audio_stats.i2s_short_writes++;
            }
            portEXIT_CRITICAL(&audio_stats_lock);

            if (err != ESP_OK)
            {
                TickType_t now = xTaskGetTickCount();
                if ((now - last_i2s_error_log_at) >= pdMS_TO_TICKS(1000))
                {
                    ESP_LOGE(TAG,
                             "I2S port %d write failed: requested=%u, "
                             "written=%u, error=%s",
                             I2S_PORT,
                             (unsigned)received,
                             (unsigned)bytes_written,
                             esp_err_to_name(err));
                    last_i2s_error_log_at = now;
                    status_led_signal_error();
                }
            }
            else if (bytes_written != received)
            {
                TickType_t now = xTaskGetTickCount();
                if ((now - last_short_write_log_at) >= pdMS_TO_TICKS(1000))
                {
                    ESP_LOGW(TAG,
                             "Short I2S write on port %d: %u/%u bytes",
                             I2S_PORT,
                             (unsigned)bytes_written,
                             (unsigned)received);
                    last_short_write_log_at = now;
                    status_led_signal_error();
                }
            }
        }
        else if (streaming && !prefetching)
        {
            prefetching = true;
            __atomic_store_n(
                &audio_writer_prefetching,
                true,
                __ATOMIC_RELAXED);

            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.buffer_underflows++;
            uint64_t underflows = audio_stats.buffer_underflows;
            portEXIT_CRITICAL(&audio_stats_lock);

            TickType_t now = xTaskGetTickCount();
            if ((now - last_underflow_log_at) >= pdMS_TO_TICKS(1000))
            {
                ESP_LOGW(TAG,
                         "Audio StreamBuffer underflow: I2S DMA is outputting "
                         "silence; returning to prefetch, underflows=%" PRIu64,
                         underflows);
                last_underflow_log_at = now;
                status_led_signal_error();
            }
        }
    }
#endif
}

static void audio_stats_task(void *argument)
{
    (void)argument;

    TickType_t last_log_at = xTaskGetTickCount();
    audio_stats_t previous_snapshot = {0};
    uint64_t last_reported_dropped_bytes = 0;
    uint64_t last_reported_trimmed_bytes = 0;
    bool was_streaming = false;
    bool a2dp_stall_reported = false;

    while (true)
    {
        if (__atomic_exchange_n(
                &first_audio_packet_log_pending,
                false,
                __ATOMIC_ACQ_REL))
        {
            audio_stats_t snapshot;
            audio_stats_snapshot(&snapshot);
            ESP_LOGI(TAG,
                     "First A2DP audio packet received: %" PRIu32 " bytes",
                     snapshot.first_packet_size);
        }

        TickType_t now = xTaskGetTickCount();
        bool streaming =
            __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);

        if (streaming && !was_streaming)
        {
            audio_stats_snapshot(&previous_snapshot);
            last_reported_dropped_bytes = previous_snapshot.dropped_bytes;
            last_reported_trimmed_bytes =
                previous_snapshot.latency_trimmed_bytes;
            last_log_at = now;
            a2dp_stall_reported = false;
        }
        else if (!streaming)
        {
            last_log_at = now;
            a2dp_stall_reported = false;
        }

        if (streaming &&
            (now - last_log_at) >=
                pdMS_TO_TICKS(AUDIO_STATS_INTERVAL_MS))
        {
            audio_stats_t snapshot;
            audio_stats_snapshot(&snapshot);
            size_t buffered = xStreamBufferBytesAvailable(audio_stream);
            uint32_t elapsed_ms =
                (uint32_t)((now - last_log_at) * portTICK_PERIOD_MS);
            uint64_t callback_delta =
                snapshot.callbacks - previous_snapshot.callbacks;
            uint64_t received_delta =
                snapshot.received_bytes - previous_snapshot.received_bytes;
            uint64_t receive_rate = elapsed_ms == 0
                                        ? 0
                                        : received_delta * 1000U / elapsed_ms;
            bool prefetching = __atomic_load_n(
                &audio_writer_prefetching,
                __ATOMIC_RELAXED);
            int8_t rssi_delta;
            bool rssi_valid;
            portENTER_CRITICAL(&rssi_lock);
            rssi_delta = bluetooth_rssi_delta;
            rssi_valid = bluetooth_rssi_delta_valid;
            portEXIT_CRITICAL(&rssi_lock);

            ESP_LOGI(TAG,
                     "Audio stats: cb=%" PRIu64 " (+%" PRIu64 ")"
                     ", rx=%" PRIu64 " (%" PRIu64 " B/s), last=%" PRIu32
                     ", queued=%" PRIu64 ", dropped=%" PRIu64
                     ", buffer=%u/%u, read=%" PRIu64
                     ", i2s=%" PRIu64 "/%" PRIu64 "ops"
                     ", diagnostic_consumed=%" PRIu64
                     ", err=%" PRIu64 ", short=%" PRIu64
                     ", gaps=%" PRIu64 " (last/max=%" PRIu32 "/%" PRIu32 "ms)"
                     ", underflows=%" PRIu64 ", resumes=%" PRIu64
                     ", latency_trim=%" PRIu64 "/%" PRIu64 "events"
                     ", discarded=%" PRIu64
                     ", rssi_delta=%d dB (%s), mode=%s",
                     snapshot.callbacks,
                     callback_delta,
                     snapshot.received_bytes,
                     receive_rate,
                     snapshot.last_packet_size,
                     snapshot.queued_bytes,
                     snapshot.dropped_bytes,
                     (unsigned)buffered,
                     (unsigned)AUDIO_STREAM_BUFFER_SIZE,
                     snapshot.stream_read_bytes,
                     snapshot.i2s_written_bytes,
                     snapshot.i2s_write_operations,
                     snapshot.diagnostic_consumed_bytes,
                     snapshot.i2s_write_errors,
                     snapshot.i2s_short_writes,
                     snapshot.pcm_gap_events,
                     snapshot.last_pcm_gap_ms,
                     snapshot.max_pcm_gap_ms,
                     snapshot.buffer_underflows,
                     snapshot.prefetch_resumes,
                     snapshot.latency_trimmed_bytes,
                     snapshot.latency_trim_events,
                     snapshot.discarded_while_stopped_bytes,
                     (int)rssi_delta,
                     rssi_valid ? "valid" : "pending",
                     AUDIO_DIAGNOSTIC_DRAIN_PCM
                         ? (AUDIO_DIAGNOSTIC_BYPASS_I2S
                                ? "diagnostic-no-i2s"
                                : "diagnostic-i2s-clocks-only")
                         : (prefetching ? "prefetching" : "playing"));

            if (snapshot.dropped_bytes > last_reported_dropped_bytes)
            {
                ESP_LOGW(TAG,
                         "Audio StreamBuffer overflow: dropped_since_last=%"
                         PRIu64 ", dropped_total=%" PRIu64 " bytes",
                         snapshot.dropped_bytes - last_reported_dropped_bytes,
                         snapshot.dropped_bytes);
                status_led_signal_error();
            }
            last_reported_dropped_bytes = snapshot.dropped_bytes;

            if (snapshot.latency_trimmed_bytes >
                last_reported_trimmed_bytes)
            {
                ESP_LOGW(TAG,
                         "Audio latency limited: trimmed_oldest_since_last=%"
                         PRIu64 ", trimmed_total=%" PRIu64
                         " bytes, events=%" PRIu64,
                         snapshot.latency_trimmed_bytes -
                             last_reported_trimmed_bytes,
                         snapshot.latency_trimmed_bytes,
                         snapshot.latency_trim_events);
                status_led_signal_error();
            }
            last_reported_trimmed_bytes =
                snapshot.latency_trimmed_bytes;

            if (callback_delta == 0 && snapshot.callbacks > 0)
            {
                if (!a2dp_stall_reported)
                {
                    ESP_LOGW(TAG,
                             "A2DP PCM stalled: no data callback during the "
                             "statistics interval");
                    a2dp_stall_reported = true;
                    status_led_signal_error();
                }
            }
            else
            {
                if (a2dp_stall_reported)
                {
                    ESP_LOGI(TAG,
                             "A2DP PCM callback recovered: +%" PRIu64
                             " callbacks",
                             callback_delta);
                }
                a2dp_stall_reported = false;
            }

            previous_snapshot = snapshot;
            last_log_at = now;
        }

        was_streaming = streaming;
        vTaskDelay(pdMS_TO_TICKS(AUDIO_STATS_POLL_MS));
    }
}

/* The Bluedroid internal SBC codec calls this with decoded 16-bit stereo PCM. */
void audio_pipeline_receive_pcm(const uint8_t *data, uint32_t length)
{
    if (data == NULL || length == 0 || audio_stream == NULL)
    {
        return;
    }

    TickType_t callback_tick = xTaskGetTickCount();
    bool reset_gap = __atomic_exchange_n(
        &pcm_gap_reset_requested,
        false,
        __ATOMIC_ACQ_REL);
    bool streaming =
        __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);
    size_t sent = xStreamBufferSend(audio_stream, data, length, 0);

    portENTER_CRITICAL(&audio_stats_lock);
    bool is_first_packet = audio_stats.callbacks == 0;
    if (is_first_packet)
    {
        audio_stats.first_packet_size = length;
    }
    audio_stats.callbacks++;
    audio_stats.received_bytes += length;
    audio_stats.last_packet_size = length;
    audio_stats.queued_bytes += sent;
    audio_stats.dropped_bytes += length - sent;

    if (!reset_gap && streaming && audio_stats.callbacks > 1)
    {
        uint32_t gap_ms =
            (uint32_t)((callback_tick - last_pcm_callback_tick) *
                       portTICK_PERIOD_MS);
        if (gap_ms >= AUDIO_PCM_GAP_THRESHOLD_MS)
        {
            audio_stats.pcm_gap_events++;
            audio_stats.last_pcm_gap_ms = gap_ms;
            if (gap_ms > audio_stats.max_pcm_gap_ms)
            {
                audio_stats.max_pcm_gap_ms = gap_ms;
            }
        }
    }
    last_pcm_callback_tick = callback_tick;
    portEXIT_CRITICAL(&audio_stats_lock);

    if (is_first_packet)
    {
        __atomic_store_n(
            &first_audio_packet_log_pending,
            true,
            __ATOMIC_RELEASE);
    }
}

esp_err_t audio_pipeline_init(void)
{
    audio_stream = xStreamBufferCreate(AUDIO_STREAM_BUFFER_SIZE, 1);
    if (audio_stream == NULL)
    {
        ESP_LOGE(TAG, "Cannot allocate audio stream buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Audio StreamBuffer created: capacity=%u bytes, trigger=1 byte, "
             "prefetch=%u bytes, latency_limit=%u->%u bytes",
             (unsigned)AUDIO_STREAM_BUFFER_SIZE,
             (unsigned)AUDIO_PREFETCH_THRESHOLD,
             (unsigned)AUDIO_LATENCY_HIGH_WATER,
             (unsigned)AUDIO_LATENCY_TARGET);

    esp_err_t err = i2s_output_init();
    if (err != ESP_OK)
    {
        return err;
    }

#if AUDIO_DIAGNOSTIC_DRAIN_PCM
    const char *audio_task_name = "audio_drain";
#else
    const char *audio_task_name = "audio_writer";
#endif

    BaseType_t result = xTaskCreatePinnedToCore(
        audio_writer_task,
        audio_task_name,
        AUDIO_WRITER_TASK_STACK_SIZE,
        NULL,
        AUDIO_WRITER_TASK_PRIORITY,
        NULL,
        AUDIO_WRITER_TASK_CORE);
    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Cannot create %s task", audio_task_name);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Task created: %s, stack=%u, priority=%d, core=%d, "
             "i2s_bypass=%s, pcm_to_i2s=%s "
             "(Bluetooth stack remains on core 0)",
             audio_task_name,
             (unsigned)AUDIO_WRITER_TASK_STACK_SIZE,
             AUDIO_WRITER_TASK_PRIORITY,
             AUDIO_WRITER_TASK_CORE,
             AUDIO_DIAGNOSTIC_BYPASS_I2S ? "yes" : "no",
             AUDIO_DIAGNOSTIC_DRAIN_PCM ? "no" : "yes");

    result = xTaskCreate(
        audio_stats_task,
        "audio_stats",
        4096,
        NULL,
        AUDIO_STATS_TASK_PRIORITY,
        NULL);
    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Cannot create audio statistics task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Task created: audio_stats, priority=%d, interval=%u ms",
             AUDIO_STATS_TASK_PRIORITY,
             (unsigned)AUDIO_STATS_INTERVAL_MS);

    return ESP_OK;
}
