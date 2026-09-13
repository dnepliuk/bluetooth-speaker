#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "app_diagnostics.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "i2s_output.h"
#include "status_led.h"

#if AUDIO_DIAGNOSTIC_TONE_ENABLED
#include <math.h>
#endif

static const char *TAG = APP_LOG_TAG;

typedef struct
{
    uint64_t callbacks;
    uint64_t received_bytes;
    uint64_t queued_bytes;
    uint64_t dropped_bytes;
    uint64_t invalid_pcm_bytes;
    size_t buffer_high_water;
    uint32_t first_packet_size;
    uint32_t last_packet_size;
    uint64_t stream_read_bytes;
    uint64_t i2s_written_bytes;
    uint64_t i2s_write_operations;
    uint64_t i2s_write_errors;
    uint64_t i2s_short_writes;
    uint64_t i2s_write_time_us;
    uint32_t max_i2s_write_time_us;
    uint64_t diagnostic_consumed_bytes;
    uint64_t buffer_underflows;
    uint64_t prefetch_resumes;
    uint64_t latency_trim_events;
    uint64_t latency_trimmed_bytes;
    uint64_t discarded_while_stopped_bytes;
    uint64_t pcm_gap_events;
    uint32_t last_pcm_gap_ms;
    uint32_t max_pcm_gap_ms;
    uint64_t gap_buckets[5];
    uint64_t prefetch_starts;
    size_t buffer_low_water_playing;
    bool buffer_low_water_valid;
    uint32_t max_callback_us;
    /* Snapshot-only values; timing state below shares audio_stats_lock. */
    uint64_t stream_epoch;
    uint64_t gap_excess_us;
    uint64_t no_pcm_us;
    bool streaming;
    uint32_t source_sample_rate;
} audio_stats_t;

static StreamBufferHandle_t audio_stream;
static volatile uint32_t requested_sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
static volatile uint32_t applied_sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
static volatile uint8_t pcm_channels = 2;
static volatile bool audio_stream_active;
static volatile bool audio_writer_prefetching = true;
static volatile bool first_audio_packet_log_pending;

static portMUX_TYPE audio_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_stats_t audio_stats;
static uint64_t stream_epoch;
static int64_t pcm_gap_anchor_us;
static bool have_stream_pcm;
static uint64_t closed_gap_excess_us;
static portMUX_TYPE rssi_lock = portMUX_INITIALIZER_UNLOCKED;
static int8_t bluetooth_rssi_delta;
static bool bluetooth_rssi_delta_valid;

/* Time beyond 30 ms in a PCM-free span, not an estimate of lost audio. */
static uint64_t gap_excess(int64_t gap_us)
{
    return gap_us > 30000 ? (uint64_t)(gap_us - 30000) : 0;
}

static void audio_stats_snapshot(audio_stats_t *snapshot)
{
    portENTER_CRITICAL(&audio_stats_lock);
    *snapshot = audio_stats;
    snapshot->streaming = __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);
    snapshot->stream_epoch = stream_epoch;
    snapshot->source_sample_rate = __atomic_load_n(
        &requested_sample_rate_hz, __ATOMIC_RELAXED);
    snapshot->no_pcm_us = snapshot->streaming
        ? (uint64_t)(esp_timer_get_time() - pcm_gap_anchor_us) : 0;
    snapshot->gap_excess_us = closed_gap_excess_us + gap_excess(snapshot->no_pcm_us);
    portEXIT_CRITICAL(&audio_stats_lock);
}

void audio_pipeline_set_streaming(bool streaming)
{
    portENTER_CRITICAL(&audio_stats_lock);
    bool was_streaming = __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);
    if (streaming != was_streaming)
    {
        int64_t now_us = esp_timer_get_time();
        if (was_streaming)
        {
            closed_gap_excess_us += gap_excess(now_us - pcm_gap_anchor_us);
        }
        pcm_gap_anchor_us = now_us;
        have_stream_pcm = false;
        ++stream_epoch;
        __atomic_store_n(&audio_stream_active, streaming, __ATOMIC_RELAXED);
    }
    portEXIT_CRITICAL(&audio_stats_lock);
}

void audio_pipeline_set_sample_rate(uint32_t sample_rate_hz)
{
    portENTER_CRITICAL(&audio_stats_lock);
    if (__atomic_load_n(&requested_sample_rate_hz, __ATOMIC_RELAXED) != sample_rate_hz)
    {
        ++stream_epoch;
    }
    __atomic_store_n(
        &requested_sample_rate_hz,
        sample_rate_hz,
        __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&audio_stats_lock);
}

void audio_pipeline_set_pcm_channels(uint8_t channels)
{
    __atomic_store_n(&pcm_channels, channels, __ATOMIC_RELAXED);
}

void audio_pipeline_update_rssi(int8_t rssi_delta, bool valid)
{
    portENTER_CRITICAL(&rssi_lock);
    bluetooth_rssi_delta = rssi_delta;
    bluetooth_rssi_delta_valid = valid;
    portEXIT_CRITICAL(&rssi_lock);
}

#if !AUDIO_DIAGNOSTIC_DRAIN_PCM
/* Only the audio writer owns this function and the I2S channel. */
static void audio_write_all(const uint8_t *data, size_t size)
{
    static TickType_t last_error_log_at;
    size_t offset = 0;
    configASSERT(size % AUDIO_PCM_FRAME_BYTES == 0);

    while (offset < size)
    {
        if (!AUDIO_DIAGNOSTIC_TONE_ENABLED &&
            !__atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED))
        {
            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.discarded_while_stopped_bytes += size - offset;
            portEXIT_CRITICAL(&audio_stats_lock);
            return;
        }

        size_t remaining = size - offset;
        size_t written = 0;
        int64_t started_us = esp_timer_get_time();
        esp_err_t err = i2s_output_write(data + offset, remaining, &written);
        uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);

        /* With aligned DMA descriptors, IDF must return whole PCM frames.
         * Fail visibly if that contract breaks; never continue at a bad offset. */
        if (written > remaining || written % AUDIO_PCM_FRAME_BYTES != 0)
        {
            ESP_LOGE(TAG, "Invalid I2S byte count: %u/%u; writer halted",
                     (unsigned)written, (unsigned)remaining);
            status_led_set_state(STATUS_LED_ERROR);
            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.i2s_write_errors++;
            portEXIT_CRITICAL(&audio_stats_lock);
            vTaskSuspend(NULL);
            return;
        }

        portENTER_CRITICAL(&audio_stats_lock);
        audio_stats.i2s_write_operations++;
        audio_stats.i2s_written_bytes += written;
        audio_stats.i2s_write_time_us += elapsed_us;
        if (elapsed_us > audio_stats.max_i2s_write_time_us)
        {
            audio_stats.max_i2s_write_time_us = elapsed_us;
        }
        if (err != ESP_OK)
        {
            audio_stats.i2s_write_errors++;
        }
        if (written != remaining)
        {
            audio_stats.i2s_short_writes++;
        }
        uint64_t problems = audio_stats.i2s_write_errors + audio_stats.i2s_short_writes;
        portEXIT_CRITICAL(&audio_stats_lock);

        /* Even a timed-out call can have written a prefix. Retry only its tail,
         * before receiving another block or applying emergency trim. */
        offset += written;
        if (err != ESP_OK || written != remaining)
        {
            TickType_t now = xTaskGetTickCount();
            if (last_error_log_at == 0 || problems == 1 ||
                (now - last_error_log_at) >= pdMS_TO_TICKS(1000))
            {
                ESP_LOGW(TAG,
                         "I2S port %d write: %u/%u bytes, error=%s, pending=%u",
                         I2S_PORT, (unsigned)written, (unsigned)remaining,
                         esp_err_to_name(err), (unsigned)(size - offset));
                last_error_log_at = now;
                status_led_signal_error();
            }
            if (written == 0)
            {
                /* Error/no-progress backoff only; never delay a successful write. */
                vTaskDelay(1);
            }
        }
    }
}
#endif

static void audio_writer_task(void *argument)
{
    (void)argument;
    app_diagnostics_track_task(DIAG_TASK_WRITER);

#if AUDIO_DIAGNOSTIC_TONE_ENABLED
    /* 441 frames contain exactly ten periods of a 1000 Hz sine at 44100 Hz.
     * Compute once; replay continuously with identical left/right samples. */
    int16_t tone[441U * 2U];
    for (size_t frame = 0; frame < 441U; ++frame)
    {
        int16_t sample = (int16_t)(3276.0f * sinf(
            2.0f * 3.14159265358979323846f * 1000.0f * (float)frame / 44100.0f));
        tone[2U * frame] = sample;
        tone[2U * frame + 1U] = sample;
    }
    ESP_LOGW(TAG, "Diagnostic tone: 1000 Hz, 44100 Hz s16le stereo, amplitude=10%%");
    while (true)
    {
        audio_write_all((const uint8_t *)tone, sizeof(tone));
    }
#elif AUDIO_DIAGNOSTIC_DRAIN_PCM
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
    TickType_t last_underflow_log_at = 0;
    TickType_t last_prefetch_log_at = 0;
    bool prefetching = true;
    bool was_streaming = false;

    while (true)
    {
        TickType_t loop_started_at = xTaskGetTickCount();
        uint32_t new_sample_rate_hz =
            __atomic_load_n(&requested_sample_rate_hz, __ATOMIC_RELAXED);
        if (new_sample_rate_hz !=
                __atomic_load_n(&applied_sample_rate_hz, __ATOMIC_RELAXED) &&
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
                __atomic_store_n(
                    &applied_sample_rate_hz, new_sample_rate_hz, __ATOMIC_RELAXED);
                next_rate_retry_at = 0;
            }
        }

        bool streaming =
            __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);
        if (streaming && !was_streaming)
        {
            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.prefetch_starts++;
            portEXIT_CRITICAL(&audio_stats_lock);
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
            buffered_before_read >= AUDIO_LATENCY_HIGH_WATER)
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
            portENTER_CRITICAL(&audio_stats_lock);
            if (!audio_stats.buffer_low_water_valid ||
                buffered_before_read < audio_stats.buffer_low_water_playing)
            {
                audio_stats.buffer_low_water_playing = buffered_before_read;
                audio_stats.buffer_low_water_valid = true;
            }
            portEXIT_CRITICAL(&audio_stats_lock);
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

            audio_write_all(write_buffer, received);
        }
        else if (streaming && !prefetching &&
                 __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED))
        {
            prefetching = true;
            __atomic_store_n(
                &audio_writer_prefetching,
                true,
                __ATOMIC_RELAXED);

            portENTER_CRITICAL(&audio_stats_lock);
            audio_stats.buffer_underflows++;
            audio_stats.prefetch_starts++;
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
    app_diagnostics_track_task(DIAG_TASK_STATS);

    int64_t last_log_us = esp_timer_get_time();
    audio_stats_t previous_snapshot = {0};
    uint64_t last_reported_dropped_bytes = 0;
    uint64_t last_reported_trimmed_bytes = 0;
    bool a2dp_stall_reported = false;
    uint64_t low_rx_intervals = 0;
    uint64_t eligible_rx_intervals = 0;

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

        int64_t now_us = esp_timer_get_time();
        bool streaming =
            __atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED);

        if (!streaming)
        {
            a2dp_stall_reported = false;
        }

        if (now_us - last_log_us >= (int64_t)AUDIO_STATS_INTERVAL_MS * 1000)
        {
            audio_stats_t snapshot;
            audio_stats_snapshot(&snapshot);
            size_t buffered = xStreamBufferBytesAvailable(audio_stream);
            uint64_t elapsed_us = (uint64_t)(now_us - last_log_us);
            uint64_t callback_delta =
                snapshot.callbacks - previous_snapshot.callbacks;
            uint64_t received_delta =
                snapshot.received_bytes - previous_snapshot.received_bytes;
            uint64_t written_delta =
                snapshot.i2s_written_bytes - previous_snapshot.i2s_written_bytes;
            uint64_t operation_delta =
                snapshot.i2s_write_operations - previous_snapshot.i2s_write_operations;
            uint64_t write_time_delta =
                snapshot.i2s_write_time_us - previous_snapshot.i2s_write_time_us;
            uint64_t receive_rate = received_delta * 1000000U / elapsed_us;
            uint64_t write_rate = written_delta * 1000000U / elapsed_us;
            uint64_t average_write_us = operation_delta == 0
                                            ? 0 : write_time_delta / operation_delta;
            uint32_t sample_rate = __atomic_load_n(
                &applied_sample_rate_hz, __ATOMIC_RELAXED);
            bool prefetching = __atomic_load_n(
                &audio_writer_prefetching,
                __ATOMIC_RELAXED);
            int8_t rssi_delta;
            bool rssi_valid;
            portENTER_CRITICAL(&rssi_lock);
            rssi_delta = bluetooth_rssi_delta;
            rssi_valid = bluetooth_rssi_delta_valid;
            portEXIT_CRITICAL(&rssi_lock);

            bool full_started_interval = !AUDIO_DIAGNOSTIC_TONE_ENABLED &&
                previous_snapshot.streaming && snapshot.streaming &&
                previous_snapshot.stream_epoch == snapshot.stream_epoch;
            uint64_t expected_source_rate =
                (uint64_t)snapshot.source_sample_rate * AUDIO_PCM_FRAME_BYTES;
            if (full_started_interval)
            {
                eligible_rx_intervals++;
                if (received_delta * 1000000U * 100U <
                    expected_source_rate * elapsed_us * 95U)
                {
                    low_rx_intervals++;
                }
            }

            ESP_LOGI(TAG,
                     "Audio stats: cb=%" PRIu64 " (+%" PRIu64 ")"
                     ", rx=%" PRIu64 ", rx_interval=%" PRIu64
                     ", rx_rate=%" PRIu64 " B/s, last=%" PRIu32
                     ", queued=%" PRIu64 ", dropped=%" PRIu64
                     ", invalid_pcm=%" PRIu64
                     ", buffer=%u/%u, buffer_max=%u, read=%" PRIu64
                     ", i2s=%" PRIu64 "/%" PRIu64 "ops"
                     ", i2s_interval=%" PRIu64 ", i2s_rate=%" PRIu64 " B/s"
                     ", write_us(avg/max)=%" PRIu64 "/%" PRIu32
                     ", diagnostic_consumed=%" PRIu64
                     ", err=%" PRIu64 ", short=%" PRIu64
                     ", gaps=%" PRIu64 " (last/max=%" PRIu32 "/%" PRIu32 "ms)"
                     ", underflows=%" PRIu64 ", resumes=%" PRIu64
                     ", latency_trim=%" PRIu64 "/%" PRIu64 "events"
                     ", discarded=%" PRIu64
                     ", rssi_delta=%d dB (%s), sample_rate=%" PRIu32
                     ", expected_rate=%" PRIu32 " B/s, audio_state=%s, mode=%s",
                     snapshot.callbacks,
                     callback_delta,
                     snapshot.received_bytes,
                     received_delta,
                     receive_rate,
                     snapshot.last_packet_size,
                     snapshot.queued_bytes,
                     snapshot.dropped_bytes,
                     snapshot.invalid_pcm_bytes,
                     (unsigned)buffered,
                     (unsigned)AUDIO_STREAM_BUFFER_SIZE,
                     (unsigned)snapshot.buffer_high_water,
                     snapshot.stream_read_bytes,
                     snapshot.i2s_written_bytes,
                     snapshot.i2s_write_operations,
                     written_delta,
                     write_rate,
                     average_write_us,
                     snapshot.max_i2s_write_time_us,
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
                     sample_rate,
                     sample_rate * AUDIO_PCM_FRAME_BYTES,
                     AUDIO_DIAGNOSTIC_TONE_ENABLED ? "disabled-tone"
                         : (streaming ? "started" : "suspended/disconnected"),
                     AUDIO_DIAGNOSTIC_TONE_ENABLED ? "diagnostic-tone"
                         : AUDIO_DIAGNOSTIC_DRAIN_PCM
                         ? (AUDIO_DIAGNOSTIC_BYPASS_I2S
                                ? "diagnostic-no-i2s"
                                : "diagnostic-i2s-clocks-only")
                         : (!streaming ? "stopped"
                             : (prefetching ? "prefetching" : "playing")));

            ESP_LOGI(TAG,
                     "PCM timing: no_pcm_now_ms=%" PRIu64
                     ", gap_excess_over_30ms_total=%" PRIu64 "ms (+%" PRIu64 "ms)"
                     ", gap_buckets_total(<30/30-50/50-100/100-200/>200ms)="
                     "%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                     ", callback_copy_lock_max_us=%" PRIu32
                     ", buffer_min_playing=%d, prefetch_starts=%" PRIu64
                     ", rx_below_95pct_intervals=%" PRIu64 "/%" PRIu64
                     ", full_started_interval=%s",
                     snapshot.no_pcm_us / 1000U,
                     snapshot.gap_excess_us / 1000U,
                     (snapshot.gap_excess_us - previous_snapshot.gap_excess_us) / 1000U,
                     snapshot.gap_buckets[0], snapshot.gap_buckets[1],
                     snapshot.gap_buckets[2], snapshot.gap_buckets[3],
                     snapshot.gap_buckets[4], snapshot.max_callback_us,
                     snapshot.buffer_low_water_valid
                         ? (int)snapshot.buffer_low_water_playing : -1,
                     snapshot.prefetch_starts, low_rx_intervals, eligible_rx_intervals,
                     full_started_interval ? "yes" : "no");
            app_diagnostics_log();

            if (snapshot.dropped_bytes > last_reported_dropped_bytes)
            {
                ESP_LOGW(TAG,
                         "Audio PCM dropped (overflow/invalid format): since_last=%"
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

            if (!AUDIO_DIAGNOSTIC_TONE_ENABLED && snapshot.streaming &&
                snapshot.no_pcm_us >= (uint64_t)AUDIO_STATS_INTERVAL_MS * 1000U)
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
            last_log_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_STATS_POLL_MS));
    }
}

/* Bluedroid provides native signed 16-bit PCM (little-endian on ESP32).
 * Its channel count follows SBC negotiation; only stereo enters this buffer. */
void audio_pipeline_receive_pcm(const uint8_t *data, uint32_t length)
{
    if (AUDIO_DIAGNOSTIC_TONE_ENABLED ||
        data == NULL || length == 0 || audio_stream == NULL)
    {
        return;
    }

    int64_t callback_started_us = esp_timer_get_time();
    bool valid_pcm = length % AUDIO_PCM_FRAME_BYTES == 0 &&
                     __atomic_load_n(&pcm_channels, __ATOMIC_RELAXED) == 2U;
    size_t sent = 0;
    if (valid_pcm)
    {
        /* One producer / one consumer. The consumer can only increase space
         * between this query and send. Never enqueue a partial stereo frame. */
        size_t to_send = xStreamBufferSpacesAvailable(audio_stream);
        to_send -= to_send % AUDIO_PCM_FRAME_BYTES;
        if (to_send > length)
        {
            to_send = length;
        }
        if (to_send > 0)
        {
            sent = xStreamBufferSend(audio_stream, data, to_send, 0);
        }
    }
    size_t buffered = xStreamBufferBytesAvailable(audio_stream);

    portENTER_CRITICAL(&audio_stats_lock);
    int64_t now_us = esp_timer_get_time();
    uint32_t callback_us = (uint32_t)(now_us - callback_started_us);
    if (callback_us > audio_stats.max_callback_us)
    {
        audio_stats.max_callback_us = callback_us;
    }
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
    if (!valid_pcm)
    {
        /* Reject malformed packets before enqueueing; no byte/channel guessing. */
        audio_stats.invalid_pcm_bytes += length;
    }
    if (buffered > audio_stats.buffer_high_water)
    {
        audio_stats.buffer_high_water = buffered;
    }

    if (__atomic_load_n(&audio_stream_active, __ATOMIC_RELAXED))
    {
        int64_t gap_us = now_us - pcm_gap_anchor_us;
        closed_gap_excess_us += gap_excess(gap_us);
        if (have_stream_pcm)
        {
            unsigned bucket = gap_us < 30000 ? 0 : gap_us < 50000 ? 1 :
                              gap_us < 100000 ? 2 : gap_us <= 200000 ? 3 : 4;
            audio_stats.gap_buckets[bucket]++;
            uint32_t gap_ms = (uint32_t)(gap_us / 1000);
            if (gap_ms >= AUDIO_PCM_GAP_THRESHOLD_MS)
            {
                audio_stats.pcm_gap_events++;
                audio_stats.last_pcm_gap_ms = gap_ms;
            }
            if (gap_ms > audio_stats.max_pcm_gap_ms)
            {
                audio_stats.max_pcm_gap_ms = gap_ms;
            }
        }
        have_stream_pcm = true;
        pcm_gap_anchor_us = now_us;
    }
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
    audio_stream = xStreamBufferCreate(AUDIO_STREAM_BUFFER_SIZE, AUDIO_PCM_FRAME_BYTES);
    if (audio_stream == NULL)
    {
        ESP_LOGE(TAG, "Cannot allocate audio stream buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Audio StreamBuffer created: capacity=%u bytes, trigger=4 bytes, "
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

#if AUDIO_DIAGNOSTIC_TONE_ENABLED
    const char *audio_task_name = "audio_tone";
#elif AUDIO_DIAGNOSTIC_DRAIN_PCM
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
