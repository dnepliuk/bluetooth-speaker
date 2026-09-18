#include "control_transport.h"
#include "control_variant.h"
#include "control_audio_config.h"

#include <inttypes.h>
#include <stdatomic.h>
#if CONTROL_CLOCKED
#include <string.h>
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONTROL_SPSC
#include "control_spsc.h"
/* Compile-time backend substitution keeps the reference writer and callback.
 * No control_stream.c or direct FreeRTOS StreamBuffer include for variants 7/8.
 */
#define control_stream_init control_spsc_init
#define control_stream_send control_spsc_send
#define control_stream_receive(data, size, timeout) control_spsc_receive(data, size)
#define control_stream_fill control_spsc_consumer_fill
#elif CONTROL_HAS_STREAM
#include "control_stream.h"
#endif
#if CONTROL_HAS_I2S
#include "control_i2s.h"
#endif

typedef struct {
    uint64_t queued, consumed, dropped, discarded, invalid;
    uint64_t sends, send_us;
    uint64_t written, writes, write_us, errors, shorts, read_timeouts;
    uint32_t send_max_us, write_max_us;
    size_t fill_min, fill_max;
#if CONTROL_SPSC
    uint64_t overflows;
    size_t fill_last;
#endif
#if CONTROL_CLOCKED
    uint64_t prefill_completions, pcm_played, silence_inserted;
    uint64_t silence_only_blocks, partial_pcm_blocks, full_pcm_blocks, underflows;
    uint64_t reads, read_us, unaligned_reads;
    uint32_t read_max_us;
    bool prefill_active;
#endif
} transport_stats_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static transport_stats_t s_stats;
static bool s_streaming;
static bool s_stereo = true;
static uint32_t s_rate = CONTROL_SAMPLE_RATE;
static uint32_t s_epoch;
static TaskHandle_t s_init_waiter;
static esp_err_t s_init_result;
static int64_t s_previous_us;
static transport_stats_t s_previous;
static bool s_have_previous, s_previous_streaming;
static uint32_t s_previous_epoch;

#if CONTROL_HAS_STREAM
/* A single PCM producer publishes timing after ALL potentially contended work.
 * Atomic 32-bit fields avoid both torn uint64_t reads and a second timing lock.
 * Only the low-priority stats task retries a snapshot; the callback never waits.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Callback timing requires lock-free 32-bit atomics");
static _Atomic unsigned s_time_sequence, s_time_count, s_time_lo, s_time_hi, s_time_max;
static uint64_t s_previous_callback_us;
static uint32_t s_previous_callback_count;

typedef struct {
    uint64_t total_us;
    uint32_t count, max_us;
} callback_times_t;

static callback_times_t callback_times_snapshot(void)
{
    for (;;) {
        unsigned before = atomic_load(&s_time_sequence);
        if (before & 1U) {
            vTaskDelay(1);
            continue;
        }
        callback_times_t result = {
            .total_us = (uint64_t)atomic_load(&s_time_hi) << 32 |
                        atomic_load(&s_time_lo),
            .count = atomic_load(&s_time_count),
            .max_us = atomic_load(&s_time_max),
        };
        if (before == atomic_load(&s_time_sequence)) {
            return result;
        }
    }
}

void control_transport_callback_done(int64_t entry_us)
{
    /* Private to the sole A2DP producer, not accessed by the reader. */
    static uint64_t total_us;
    static uint32_t count, max_us, sequence;
    atomic_store(&s_time_sequence, ++sequence);
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - entry_us);
    total_us += elapsed;
    ++count;
    if (elapsed > max_us) {
        max_us = elapsed;
    }
    atomic_store(&s_time_lo, (uint32_t)total_us);
    atomic_store(&s_time_hi, (uint32_t)(total_us >> 32));
    atomic_store(&s_time_count, count);
    atomic_store(&s_time_max, max_us);
    atomic_store(&s_time_sequence, ++sequence);
}

/* Sampled watermarks, as in main: a woken consumer can run before this sample. */
static void record_fill_locked(size_t fill)
{
#if CONTROL_SPSC
    s_stats.fill_last = fill;
#endif
    if (fill < s_stats.fill_min) s_stats.fill_min = fill;
    if (fill > s_stats.fill_max) s_stats.fill_max = fill;
}

void control_transport_send(const uint8_t *data, uint32_t len)
{
    uint32_t send_us = 0;
    const bool valid = __atomic_load_n(&s_stereo, __ATOMIC_RELAXED) &&
                       data != NULL && len != 0 && len % CONTROL_FRAME_BYTES == 0;
    const size_t sent = valid ? control_stream_send(data, len, &send_us) : 0;
#if CONTROL_SPSC
    const size_t fill = control_spsc_producer_fill();
#else
    const size_t fill = control_stream_fill();
#endif
    portENTER_CRITICAL(&s_lock);
#if CONTROL_SPSC
    s_stats.overflows += valid && sent < len;
#endif
    s_stats.queued += sent;
    s_stats.dropped += len - sent;
    s_stats.invalid += valid ? 0 : len;
    ++s_stats.sends;
    s_stats.send_us += send_us;
    if (send_us > s_stats.send_max_us) s_stats.send_max_us = send_us;
    record_fill_locked(fill);
    portEXIT_CRITICAL(&s_lock);
}
#endif

void control_transport_set_streaming(bool streaming)
{
#if CONTROL_CLOCKED
    /* Pair state/epoch snapshots with the existing producer publication lock.
     * No StreamBuffer operation, wait or I2S access in this event callback. */
    portENTER_CRITICAL(&s_lock);
#endif
    if (__atomic_exchange_n(&s_streaming, streaming, __ATOMIC_RELAXED) != streaming) {
        __atomic_fetch_add(&s_epoch, 1U, __ATOMIC_RELAXED);
    }
#if CONTROL_CLOCKED
    portEXIT_CRITICAL(&s_lock);
#endif
}

void control_transport_set_format(uint32_t rate, bool stereo)
{
#if CONTROL_CLOCKED
    portENTER_CRITICAL(&s_lock);
#endif
    bool changed = __atomic_exchange_n(&s_rate, rate, __ATOMIC_RELAXED) != rate;
    changed |= __atomic_exchange_n(&s_stereo, stereo, __ATOMIC_RELAXED) != stereo;
    if (changed) __atomic_fetch_add(&s_epoch, 1U, __ATOMIC_RELAXED);
#if CONTROL_CLOCKED
    portEXIT_CRITICAL(&s_lock);
#endif
}

#if CONTROL_HAS_I2S && !CONTROL_CLOCKED
static void write_all(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
#if CONTROL_HAS_STREAM
        if (!__atomic_load_n(&s_streaming, __ATOMIC_RELAXED)) {
            portENTER_CRITICAL(&s_lock);
            s_stats.discarded += size - offset;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
#endif
        size_t written = 0;
        const size_t remaining = size - offset;
        const int64_t start_us = esp_timer_get_time();
        const esp_err_t err = control_i2s_write(data + offset, remaining, &written);
        const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start_us);
        const bool invalid = written > remaining || written % CONTROL_FRAME_BYTES != 0;
        portENTER_CRITICAL(&s_lock);
        ++s_stats.writes;
        s_stats.written += invalid ? 0 : written;
        s_stats.write_us += elapsed;
        s_stats.errors += err != ESP_OK || invalid;
        s_stats.shorts += written != remaining;
        if (elapsed > s_stats.write_max_us) s_stats.write_max_us = elapsed;
        portEXIT_CRITICAL(&s_lock);
        /* Never continue from a corrupt frame offset; error remains in stats. */
        if (invalid) {
            vTaskSuspend(NULL);
            return;
        }
        offset += written;  /* Preserve partial progress even on timeout. */
        if (written == 0) vTaskDelay(1);
    }
}
#endif

#if CONTROL_CLOCKED
typedef struct {
    uint64_t queued, sends;
    uint32_t epoch, rate;
    bool streaming, stereo;
} clocked_state_t;

static clocked_state_t clocked_state(void)
{
    portENTER_CRITICAL(&s_lock);
    clocked_state_t state = {
        .queued = s_stats.queued, .sends = s_stats.sends,
        .epoch = s_epoch, .rate = s_rate,
        .streaming = s_streaming, .stereo = s_stereo,
    };
    portEXIT_CRITICAL(&s_lock);
    return state;
}

static size_t clocked_read(uint8_t *data, size_t size, uint64_t *consumed)
{
    const int64_t started = esp_timer_get_time();
    const size_t received = control_stream_receive(data, size, 0);
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    const size_t fill = control_stream_fill();
    *consumed += received;
    portENTER_CRITICAL(&s_lock);
    s_stats.consumed += received;
    ++s_stats.reads;
    s_stats.read_us += elapsed;
    s_stats.unaligned_reads += received % CONTROL_FRAME_BYTES != 0;
    if (elapsed > s_stats.read_max_us) s_stats.read_max_us = elapsed;
    record_fill_locked(fill);
    portEXIT_CRITICAL(&s_lock);
    return received;
}

/* Only the reader advances the tail. Drain a finite, already-published prefix;
 * later producer data stays queued. Never reset a live StreamBuffer. */
static void clocked_drain(uint8_t *chunk, uint64_t cutoff, uint64_t *consumed)
{
    while (*consumed < cutoff) {
        const uint64_t pending = cutoff - *consumed;
        const size_t size = pending < CONTROL_CHUNK_BYTES ? pending : CONTROL_CHUNK_BYTES;
        const size_t received = clocked_read(chunk, size, consumed);
        portENTER_CRITICAL(&s_lock);
        s_stats.discarded += received;
        portEXIT_CRITICAL(&s_lock);
        if (received == 0) break;
    }
}

static void clocked_write(uint8_t *chunk, size_t pcm_size, uint32_t epoch)
{
    size_t offset = 0, pcm_written = 0;
    while (offset < CONTROL_CHUNK_BYTES) {
        /* A transition cancels the unsent PCM prefix, including partial retries.
         * An already-running driver call/DMA descriptor cannot be retracted. */
        if (offset < pcm_size && __atomic_load_n(&s_epoch, __ATOMIC_RELAXED) != epoch) {
            memset(chunk + offset, 0, pcm_size - offset);
            portENTER_CRITICAL(&s_lock);
            s_stats.discarded += pcm_size - offset;
            portEXIT_CRITICAL(&s_lock);
            pcm_size = offset;
        }
        size_t written = 0;
        const size_t remaining = CONTROL_CHUNK_BYTES - offset;
        const int64_t started = esp_timer_get_time();
        const esp_err_t err = control_i2s_write(chunk + offset, remaining, &written);
        const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        const bool invalid = written > remaining || written % CONTROL_FRAME_BYTES != 0;
        const size_t pcm_remaining = offset < pcm_size ? pcm_size - offset : 0;
        const size_t accepted_pcm = invalid ? 0 :
                                   (written < pcm_remaining ? written : pcm_remaining);
        portENTER_CRITICAL(&s_lock);
        ++s_stats.writes;
        s_stats.written += invalid ? 0 : written;
        s_stats.write_us += elapsed;
        s_stats.errors += err != ESP_OK || invalid;
        s_stats.shorts += written != remaining;
        s_stats.pcm_played += accepted_pcm;
        s_stats.silence_inserted += invalid ? 0 : written - accepted_pcm;
        if (elapsed > s_stats.write_max_us) s_stats.write_max_us = elapsed;
        portEXIT_CRITICAL(&s_lock);
        if (invalid) {
            vTaskSuspend(NULL);  /* Broken driver contract; never shift L/R. */
            return;
        }
        pcm_written += accepted_pcm;
        offset += written;
        if (written == 0) vTaskDelay(1);  /* Error backoff only, never pacing. */
    }
    portENTER_CRITICAL(&s_lock);
    s_stats.silence_only_blocks += pcm_written == 0;
    s_stats.partial_pcm_blocks += pcm_written != 0 && pcm_written < CONTROL_CHUNK_BYTES;
    s_stats.full_pcm_blocks += pcm_written == CONTROL_CHUNK_BYTES;
    portEXIT_CRITICAL(&s_lock);
}

static void clocked_loop(void)
{
    uint8_t chunk[CONTROL_CHUNK_BYTES];
    uint8_t carry[CONTROL_FRAME_BYTES];
    size_t carry_size = 0, quiet_bytes = 0;
    uint64_t consumed = 0, fence_sends = 0;
    uint32_t epoch = UINT32_MAX, applied_rate = CONTROL_SAMPLE_RATE;
    int64_t next_rate_retry_us = 0;
    bool fence_pending = true, prefill = true;
    for (;;) {
        const clocked_state_t state = clocked_state();
        if (state.epoch != epoch) {
            epoch = state.epoch;
            fence_sends = state.sends;
            fence_pending = true;
            prefill = true;
            quiet_bytes = 0;
            portENTER_CRITICAL(&s_lock);
            s_stats.discarded += carry_size;
            portEXIT_CRITICAL(&s_lock);
            carry_size = 0;
            next_rate_retry_us = 0;
        }

        /* Seeing one subsequent completed send fences any producer that was
         * in flight at the epoch snapshot, even if it sent zero (buffer full).
         * This deliberately discards a transition prefix of new PCM as well.
         * The producer and its timing code are identical to variant 5, except
         * for the SPSC backend/counters in variants 7/8. The same finite-prefix
         * drain advances ONLY the consumer-owned read counter. Neither state
         * callbacks nor the producer reset any ring counters. */
        const bool drain = !state.streaming || !state.stereo || fence_pending;
        if (drain) {
            clocked_drain(chunk, state.queued, &consumed);
            if (state.streaming && state.stereo && state.sends != fence_sends &&
                consumed >= state.queued) {
                fence_pending = false;
            }
        }

        if (state.rate != 0 && state.rate != applied_rate &&
            esp_timer_get_time() >= next_rate_retry_us) {
            const esp_err_t err = control_i2s_set_rate(state.rate);
            if (err == ESP_OK) applied_rate = state.rate;
            else {
                portENTER_CRITICAL(&s_lock);
                ++s_stats.errors;
                portEXIT_CRITICAL(&s_lock);
            }
            next_rate_retry_us = esp_timer_get_time() + INT64_C(1000000);
        }
        const bool ready = state.streaming && state.stereo && !fence_pending &&
                           state.rate != 0 && state.rate == applied_rate;
        const size_t fill = control_stream_fill();
        /* Flush at least one complete DMA depth with zeros after a transition.
         * Prefill never blocks I2S; an underflow never re-arms prefill. */
        if (ready && prefill && fill >= CONTROL_PREFILL_BYTES &&
            quiet_bytes >= CONTROL_DMA_DESCRIPTORS * CONTROL_DMA_FRAMES * CONTROL_FRAME_BYTES) {
            prefill = false;
            portENTER_CRITICAL(&s_lock);
            ++s_stats.prefill_completions;
            portEXIT_CRITICAL(&s_lock);
        }

        size_t pcm_size = 0;
        if (ready && !prefill) {
            memcpy(chunk, carry, carry_size);
            const size_t received = clocked_read(chunk + carry_size,
                                                 sizeof(chunk) - carry_size, &consumed);
            const size_t total = carry_size + received;
            pcm_size = total - total % CONTROL_FRAME_BYTES;
            carry_size = total - pcm_size;
            memcpy(carry, chunk + pcm_size, carry_size);
            portENTER_CRITICAL(&s_lock);
            s_stats.underflows += pcm_size < sizeof(chunk);
            portEXIT_CRITICAL(&s_lock);
        }
        /* Full PCM overwrites the whole chunk; only the missing tail is zeroed. */
        memset(chunk + pcm_size, 0, sizeof(chunk) - pcm_size);
        portENTER_CRITICAL(&s_lock);
        s_stats.prefill_active = prefill;
        record_fill_locked(fill);
        portEXIT_CRITICAL(&s_lock);
        clocked_write(chunk, pcm_size, epoch);
        if (pcm_size == 0 && quiet_bytes <
            CONTROL_DMA_DESCRIPTORS * CONTROL_DMA_FRAMES * CONTROL_FRAME_BYTES) {
            quiet_bytes += sizeof(chunk);
        }
    }
}
#endif

static void consumer_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
#if CONTROL_HAS_I2S
    err = control_i2s_init();
#endif
    s_init_result = err;
    xTaskNotifyGive(s_init_waiter);
    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
#if CONTROL_CLOCKED
    clocked_loop();
#else
    uint8_t chunk[CONTROL_CHUNK_BYTES] = {0};
    for (;;) {
#if CONTROL_HAS_STREAM
        const size_t received = control_stream_receive(chunk, sizeof(chunk),
#if CONTROL_HAS_I2S
                                    pdMS_TO_TICKS(CONTROL_RECEIVE_TIMEOUT_MS));
#else
                                    portMAX_DELAY);
#endif
        const size_t fill = control_stream_fill();
        const bool streaming = __atomic_load_n(&s_streaming, __ATOMIC_RELAXED);
        portENTER_CRITICAL(&s_lock);
        s_stats.consumed += received;
        s_stats.read_timeouts += received == 0 && streaming;
        record_fill_locked(fill);
#if CONTROL_HAS_I2S
        s_stats.discarded += streaming ? 0 : received;
#endif
        portEXIT_CRITICAL(&s_lock);
#if CONTROL_HAS_I2S
        if (!streaming || received == 0) continue;
        const uint32_t rate = __atomic_load_n(&s_rate, __ATOMIC_RELAXED);
        err = rate == 0 ? ESP_ERR_INVALID_ARG : control_i2s_set_rate(rate);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            ++s_stats.errors;
            s_stats.discarded += received;
            portEXIT_CRITICAL(&s_lock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        write_all(chunk, received);
#endif
#else
        /* Continuous digital zeroes at fixed 44100 Hz, including during pause.
         * Blocking writes are DMA-paced; no synthetic tone or software pacing.
         */
        write_all(chunk, sizeof(chunk));
#endif
    }
#endif
}

esp_err_t control_transport_init(void)
{
#if CONTROL_HAS_STREAM
    esp_err_t err = control_stream_init();
    if (err != ESP_OK) return err;
#endif
    s_previous_us = esp_timer_get_time();
    s_init_waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(consumer_task, "control_writer", CONTROL_WRITER_STACK,
                                NULL, CONTROL_WRITER_PRIORITY, NULL,
                                CONTROL_WRITER_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_init_result != ESP_OK) return s_init_result;
#if CONTROL_SPSC
    _Static_assert(CONTROL_WRITER_PRIORITY == 22 && CONTROL_WRITER_CORE == 1 &&
                   CONTROL_WRITER_STACK == 8192U, "Keep reference writer settings");
    ESP_LOGI("CONTROL", "transport: spsc=1, i2s=1, buffer_bytes=%u, frame_bytes=%u,"
             " chunk=%u, task_priority=%d, core=%d, stack=%u",
             CONTROL_STREAM_BYTES, CONTROL_FRAME_BYTES, CONTROL_CHUNK_BYTES,
             CONTROL_WRITER_PRIORITY, CONTROL_WRITER_CORE, CONTROL_WRITER_STACK);
#else
    ESP_LOGI("CONTROL", "transport: stream=%d, i2s=%d, stream_bytes=%u, trigger=%u,"
             " chunk=%u, task_priority=%d, core=%d, stack=%u",
             CONTROL_HAS_STREAM, CONTROL_HAS_I2S, CONTROL_STREAM_BYTES,
             CONTROL_FRAME_BYTES, CONTROL_CHUNK_BYTES, CONTROL_WRITER_PRIORITY,
             CONTROL_WRITER_CORE, CONTROL_WRITER_STACK);
#endif
#if CONTROL_HAS_I2S
    ESP_LOGI("CONTROL", "I2S: init/IRQ core=1, BCK=26, WS=25, DATA=22, MCLK=unused,"
             " Philips s16le stereo, rate=44100, DMA=%ux%u frames, auto_clear=yes",
             CONTROL_DMA_DESCRIPTORS, CONTROL_DMA_FRAMES);
#endif
#if CONTROL_CLOCKED
    ESP_LOGI("CONTROL", "clocked writer: receive_timeout=0, prefill_bytes=%u,"
             " output_bytes=%u, pacing=blocking-I2S, re_prefill_on_underflow=no",
             CONTROL_PREFILL_BYTES, CONTROL_CHUNK_BYTES);
#endif
#if CONTROL_DMA_ALIGNED
    ESP_LOGI("CONTROL", "writer DMA alignment: output_bytes=%u, dma_descriptor_bytes=%u,"
             " dma_frames=%u, frame_bytes=%u, writer_dma_aligned=yes",
             CONTROL_CHUNK_BYTES, CONTROL_DMA_FRAMES * CONTROL_FRAME_BYTES,
             CONTROL_DMA_FRAMES, CONTROL_FRAME_BYTES);
#endif
    return ESP_OK;
}

void control_transport_log_stats(void)
{
    transport_stats_t current;
    portENTER_CRITICAL(&s_lock);
    const int64_t now_us = esp_timer_get_time();
    current = s_stats;
    portEXIT_CRITICAL(&s_lock);
    const uint64_t elapsed = (uint64_t)(now_us - s_previous_us);
    const uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_RELAXED);
    const bool streaming = __atomic_load_n(&s_streaming, __ATOMIC_RELAXED);
    const bool full = s_have_previous && s_previous_streaming && streaming &&
                      epoch == s_previous_epoch;
#if CONTROL_HAS_STREAM
    const callback_times_t times = callback_times_snapshot();
    const uint32_t callbacks = times.count - s_previous_callback_count;
    const uint64_t sends = current.sends - s_previous.sends;
#if CONTROL_SPSC
    static uint32_t previous_wrap_writes, previous_wrap_reads;
    const uint32_t wrap_writes = control_spsc_wrap_writes();
    const uint32_t wrap_reads = control_spsc_wrap_reads();
    const uint64_t consumer_calls = current.reads - s_previous.reads;
    ESP_LOGI("CONTROL", "SPSC: elapsed_ms=%" PRIu64 ", queued_interval=%" PRIu64
             ", consumed_interval=%" PRIu64 ", dropped_interval=%" PRIu64
             ", overflow_events=%" PRIu64 ", buffer_fill=%u/32768, buffer_min=%u, buffer_max=%u,"
             " producer_us_avg=%" PRIu64 ", producer_us_max=%" PRIu32
             ", consumer_us_avg=%" PRIu64 ", consumer_us_max=%" PRIu32
             ", wrap_writes=%" PRIu32 ", wrap_reads=%" PRIu32
             ", full_started_interval=%d, invalid_interval=%" PRIu64
             ", discarded_interval=%" PRIu64 ", callback_total_us_avg=%" PRIu64
             ", callback_total_us_max=%" PRIu32,
             elapsed / 1000, current.queued - s_previous.queued,
             current.consumed - s_previous.consumed, current.dropped - s_previous.dropped,
             current.overflows - s_previous.overflows, (unsigned)current.fill_last,
             (unsigned)current.fill_min, (unsigned)current.fill_max,
             sends ? (current.send_us - s_previous.send_us) / sends : 0, current.send_max_us,
             consumer_calls ? (current.read_us - s_previous.read_us) / consumer_calls : 0,
             current.read_max_us, (uint32_t)(wrap_writes - previous_wrap_writes),
             (uint32_t)(wrap_reads - previous_wrap_reads), full,
             current.invalid - s_previous.invalid, current.discarded - s_previous.discarded,
             callbacks ? (times.total_us - s_previous_callback_us) / callbacks : 0,
             times.max_us);
    previous_wrap_writes = wrap_writes;
    previous_wrap_reads = wrap_reads;
#else
    ESP_LOGI("CONTROL", "transport: elapsed_ms=%" PRIu64 ", full_started_interval=%d,"
             " queued_interval=%" PRIu64 ", consumed_interval=%" PRIu64
             ", dropped_interval=%" PRIu64 ", invalid_interval=%" PRIu64
             ", discarded_interval=%" PRIu64 ", buffer_fill=%u, buffer_min=%u, buffer_max=%u,"
             " read_timeouts_interval=%" PRIu64 ", callback_total_us_avg=%" PRIu64
             ", callback_total_us_max=%" PRIu32 ", stream_send_us_avg=%" PRIu64
             ", stream_send_us_max=%" PRIu32,
             elapsed / 1000, full, current.queued - s_previous.queued,
             current.consumed - s_previous.consumed, current.dropped - s_previous.dropped,
             current.invalid - s_previous.invalid, current.discarded - s_previous.discarded,
             (unsigned)control_stream_fill(), (unsigned)current.fill_min,
             (unsigned)current.fill_max, current.read_timeouts - s_previous.read_timeouts,
             callbacks ? (times.total_us - s_previous_callback_us) / callbacks : 0,
             times.max_us, sends ? (current.send_us - s_previous.send_us) / sends : 0,
             current.send_max_us);
#endif
    s_previous_callback_us = times.total_us;
    s_previous_callback_count = times.count;
#endif
#if CONTROL_HAS_I2S
    const uint64_t writes = current.writes - s_previous.writes;
    const uint64_t written = current.written - s_previous.written;
    ESP_LOGI("CONTROL", "i2s: elapsed_ms=%" PRIu64 ", full_started_interval=%d,"
             " i2s_written_interval=%" PRIu64 ", i2s_rate=%" PRIu64
             " B/s, write_operations=%" PRIu64 ", write_us_avg=%" PRIu64
             ", write_us_max=%" PRIu32 ", errors=%" PRIu64 ", short_writes=%" PRIu64,
             elapsed / 1000, full, written, written * UINT64_C(1000000) / elapsed,
             writes, writes ? (current.write_us - s_previous.write_us) / writes : 0,
             current.write_max_us, current.errors - s_previous.errors,
             current.shorts - s_previous.shorts);
#endif
#if CONTROL_CLOCKED
    const uint64_t reads = current.reads - s_previous.reads;
    ESP_LOGI("CONTROL", "clocked: elapsed_ms=%" PRIu64 ", full_started_interval=%d,"
             " prefill_active=%d, prefill_completions=%" PRIu64
             ", pcm_played_interval=%" PRIu64 ", silence_inserted_interval=%" PRIu64
             ", silence_only_blocks_interval=%" PRIu64 ", partial_pcm_blocks_interval=%" PRIu64
             ", full_pcm_blocks_interval=%" PRIu64 ", underflow_events_interval=%" PRIu64
             ", stream_read_us_avg=%" PRIu64 ", stream_read_us_max=%" PRIu32
             ", unaligned_reads_interval=%" PRIu64,
             elapsed / 1000, full, current.prefill_active, current.prefill_completions,
             current.pcm_played - s_previous.pcm_played,
             current.silence_inserted - s_previous.silence_inserted,
             current.silence_only_blocks - s_previous.silence_only_blocks,
             current.partial_pcm_blocks - s_previous.partial_pcm_blocks,
             current.full_pcm_blocks - s_previous.full_pcm_blocks,
             current.underflows - s_previous.underflows,
             reads ? (current.read_us - s_previous.read_us) / reads : 0,
             current.read_max_us, current.unaligned_reads - s_previous.unaligned_reads);
#endif
    s_previous = current;
    s_previous_us = now_us;
    s_previous_epoch = epoch;
    s_previous_streaming = streaming;
    s_have_previous = true;
}
