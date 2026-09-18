#include "control_spsc.h"
#include "control_variant.h"
#include "control_audio_config.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"

#if !CONTROL_SPSC
#error "The SPSC module belongs only to the two SPSC variants"
#endif

_Static_assert(CONTROL_STREAM_BYTES == 32768U, "Keep the A/B capacity at 32768");
_Static_assert((CONTROL_STREAM_BYTES & (CONTROL_STREAM_BYTES - 1U)) == 0,
               "Ring capacity must be a power of two");
_Static_assert(CONTROL_STREAM_BYTES < UINT32_MAX / 2U, "Bound unsigned distance");
_Static_assert(CONTROL_FRAME_BYTES == 4U &&
               CONTROL_STREAM_BYTES % CONTROL_FRAME_BYTES == 0,
               "Ring positions must preserve s16le stereo frames");
#if CONTROL_DMA_ALIGNED
_Static_assert(CONTROL_CHUNK_BYTES == 3840U &&
               CONTROL_CHUNK_BYTES % CONTROL_FRAME_BYTES == 0 &&
               CONTROL_CHUNK_BYTES <= CONTROL_STREAM_BYTES &&
               CONTROL_CHUNK_BYTES == CONTROL_DMA_FRAMES * CONTROL_FRAME_BYTES,
               "Aligned output must equal one unchanged DMA descriptor");
#else
_Static_assert(CONTROL_CHUNK_BYTES == 4096U &&
               CONTROL_CHUNK_BYTES % CONTROL_FRAME_BYTES == 0 &&
               CONTROL_CHUNK_BYTES <= CONTROL_STREAM_BYTES, "Invalid output chunk");
#endif
_Static_assert(CONTROL_PREFILL_BYTES == 8192U &&
               CONTROL_PREFILL_BYTES % CONTROL_FRAME_BYTES == 0 &&
               CONTROL_PREFILL_BYTES >= CONTROL_CHUNK_BYTES &&
               CONTROL_PREFILL_BYTES < CONTROL_STREAM_BYTES, "Invalid prefill");
_Static_assert(sizeof(uint32_t) == 4 && __atomic_always_lock_free(4, 0),
               "The installed GCC must inline aligned 32-bit atomic operations");

/* ESP-IDF places .dram1 in internal RAM, regardless of external RAM settings.
 * GCC naturally aligns uint32_t; align the byte storage to a stereo frame too.
 */
static DRAM_ATTR uint8_t s_pcm[CONTROL_STREAM_BYTES] __attribute__((aligned(4)));
static DRAM_ATTR uint32_t s_write, s_read;
static DRAM_ATTR uint32_t s_wrap_writes, s_wrap_reads;

esp_err_t control_spsc_init(void)
{
    return ESP_OK;
}

size_t control_spsc_producer_fill(void)
{
    const uint32_t write = __atomic_load_n(&s_write, __ATOMIC_RELAXED);
    const uint32_t read = __atomic_load_n(&s_read, __ATOMIC_ACQUIRE);
    return (uint32_t)(write - read);
}

size_t control_spsc_consumer_fill(void)
{
    const uint32_t read = __atomic_load_n(&s_read, __ATOMIC_RELAXED);
    const uint32_t write = __atomic_load_n(&s_write, __ATOMIC_ACQUIRE);
    return (uint32_t)(write - read);
}

size_t control_spsc_send(const uint8_t *data, uint32_t len, uint32_t *send_us)
{
    const int64_t started = esp_timer_get_time();
    size_t sent = 0;
    /* Match control_stream_send: reject the ENTIRE malformed block. Otherwise
     * accept the largest frame-aligned prefix and drop its tail, without retry.
     */
    if (data != NULL && len != 0 && len % CONTROL_FRAME_BYTES == 0) {
        const uint32_t write = __atomic_load_n(&s_write, __ATOMIC_RELAXED);
        const uint32_t read = __atomic_load_n(&s_read, __ATOMIC_ACQUIRE);
        const uint32_t free_bytes = CONTROL_STREAM_BYTES - (uint32_t)(write - read);
        sent = len < free_bytes ? len : free_bytes;
        if (sent != 0) {
            const size_t offset = write & (CONTROL_STREAM_BYTES - 1U);
            const size_t contiguous = CONTROL_STREAM_BYTES - offset;
            const size_t first = sent < contiguous ? sent : contiguous;
            memcpy(s_pcm + offset, data, first);
            if (sent > first) {
                memcpy(s_pcm, data + first, sent - first);
                const uint32_t wraps = __atomic_load_n(&s_wrap_writes, __ATOMIC_RELAXED);
                __atomic_store_n(&s_wrap_writes, wraps + 1U, __ATOMIC_RELAXED);
            }
            /* Publish only after BOTH copies. Consumer acquire sees all PCM. */
            __atomic_store_n(&s_write, write + (uint32_t)sent, __ATOMIC_RELEASE);
        }
    }
    *send_us = (uint32_t)(esp_timer_get_time() - started);
    return sent;
}

size_t control_spsc_receive(uint8_t *data, size_t size)
{
    const uint32_t read = __atomic_load_n(&s_read, __ATOMIC_RELAXED);
    const uint32_t write = __atomic_load_n(&s_write, __ATOMIC_ACQUIRE);
    const uint32_t available = (uint32_t)(write - read);
    size -= size % CONTROL_FRAME_BYTES;
    const size_t received = size < available ? size : available;
    if (data == NULL || received == 0) return 0;
    const size_t offset = read & (CONTROL_STREAM_BYTES - 1U);
    const size_t contiguous = CONTROL_STREAM_BYTES - offset;
    const size_t first = received < contiguous ? received : contiguous;
    memcpy(data, s_pcm + offset, first);
    if (received > first) {
        memcpy(data + first, s_pcm, received - first);
        const uint32_t wraps = __atomic_load_n(&s_wrap_reads, __ATOMIC_RELAXED);
        __atomic_store_n(&s_wrap_reads, wraps + 1U, __ATOMIC_RELAXED);
    }
    /* Producer acquire cannot reuse these bytes until both reads complete.
     * Unsigned subtraction/addition remains valid across UINT32_MAX: outstanding
     * distance is always <= capacity, and capacity divides the counter modulus.
     */
    __atomic_store_n(&s_read, read + (uint32_t)received, __ATOMIC_RELEASE);
    return received;
}

uint32_t control_spsc_wrap_writes(void)
{
    return __atomic_load_n(&s_wrap_writes, __ATOMIC_RELAXED);
}

uint32_t control_spsc_wrap_reads(void)
{
    return __atomic_load_n(&s_wrap_reads, __ATOMIC_RELAXED);
}
