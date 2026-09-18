/* Executes the production ring, including near-UINT32_MAX positions.
 * Only the test may seed counters while BOTH owners are stopped.
 * Windows threads stress two simultaneous owners; not an ESP32 scheduler test.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef NDEBUG
#error "These tests require active assertions, including at -O2"
#endif

static HANDLE copy_entered, copy_continue;
static const void *paused_source;
static void *ring_copy(void *dst, const void *src, size_t size)
{
    void *result = memcpy(dst, src, size);
    if (src == paused_source) {
        assert(SetEvent(copy_entered));
        assert(WaitForSingleObject(copy_continue, 30000) == WAIT_OBJECT_0);
    }
    return result;
}

#define memcpy ring_copy
#include "../src/control_spsc.c"
#undef memcpy

int64_t esp_timer_get_time(void)
{
    /* Deterministic stub; only producer calls the ring timer. */
    static int64_t ticks;
    return ++ticks;
}

static void seed(uint32_t counter)
{
    assert(counter % 4 == 0);
    __atomic_store_n(&s_write, counter, __ATOMIC_RELAXED);
    __atomic_store_n(&s_read, counter, __ATOMIC_RELAXED);
    __atomic_store_n(&s_wrap_writes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_wrap_reads, 0, __ATOMIC_RELAXED);
}

static uint32_t input[16384], output[16384];

static void boundary_tests(void)
{
    uint32_t us;
    for (size_t i = 0; i < 16384; ++i) input[i] = (uint32_t)i ^ 0xa55a1200;
    seed(0);
    assert(control_spsc_init() == ESP_OK);
    assert(control_spsc_receive((uint8_t *)output, sizeof(output)) == 0);
    assert(control_spsc_send(NULL, 4, &us) == 0);
    assert(control_spsc_send((uint8_t *)input, 0, &us) == 0);
    assert(control_spsc_send((uint8_t *)input, 7, &us) == 0);
    assert(control_spsc_producer_fill() == 0);
    assert(control_spsc_send((uint8_t *)input, 32764, &us) == 32764);
    assert(control_spsc_send((uint8_t *)input + 32764, 12, &us) == 4);
    assert(control_spsc_send((uint8_t *)input, 4096, &us) == 0);
    assert(control_spsc_producer_fill() == 32768);
    assert(control_spsc_receive(NULL, 4) == 0);
    assert(control_spsc_receive((uint8_t *)output, 3) == 0);
    assert(control_spsc_receive((uint8_t *)output, 7) == 4);
    assert(output[0] == input[0]);
    assert(control_spsc_receive((uint8_t *)output + 4, sizeof(output) - 4) == 32764);
    assert(memcmp(input, output, 32768) == 0);
    assert(control_spsc_consumer_fill() == 0);

    /* Both the physical array and the monotonic uint32 counter wrap. */
    seed(UINT32_MAX - 7U);
    assert(control_spsc_send((uint8_t *)input, 20, &us) == 20);
    assert(__atomic_load_n(&s_write, __ATOMIC_RELAXED) == 12);
    assert(control_spsc_receive((uint8_t *)output, 20) == 20);
    assert(__atomic_load_n(&s_read, __ATOMIC_RELAXED) == 12);
    assert(memcmp(input, output, 20) == 0);
    assert(control_spsc_wrap_writes() == 1 && control_spsc_wrap_reads() == 1);
    assert(control_spsc_send((uint8_t *)input, UINT32_MAX - 3U, &us) == 32768);
    assert(control_spsc_receive((uint8_t *)output, sizeof(output)) == 32768);
    assert(memcmp(input, output, 32768) == 0);
}

static uint32_t random_state = 0x157ac09b;
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void fifo_oracle_test(void)
{
    /* Flat byte FIFO is an independent oracle, not a second circular buffer. */
    uint8_t expected[32768];
    size_t length = 0;
    uint32_t us;
    seed(UINT32_MAX - 65535U);
    for (unsigned step = 0; step < 100000; ++step) {
        if (next_random() & 1U) {
            size_t n = next_random() % sizeof(input);
            for (size_t i = 0; i < (n + 3) / 4; ++i) input[i] = next_random();
            size_t accepted = n % 4 ? 0 : n;
            if (accepted > sizeof(expected) - length) accepted = sizeof(expected) - length;
            assert(control_spsc_send((uint8_t *)input, (uint32_t)n, &us) == accepted);
            memcpy(expected + length, input, accepted);
            length += accepted;
        } else {
            const size_t requested = next_random() % sizeof(output);
            size_t received = requested - requested % 4;
            if (received > length) received = length;
            assert(control_spsc_receive((uint8_t *)output, requested) == received);
            assert(memcmp(expected, output, received) == 0);
            memmove(expected, expected + received, length - received);
            length -= received;
        }
        assert(control_spsc_producer_fill() == length);
        assert(control_spsc_consumer_fill() == length);
    }
}

static void output_block_tests(void)
{
    const size_t block = CONTROL_CHUNK_BYTES;
    uint32_t us, produced = 0, expected = 0;
    seed(UINT32_MAX - 32767U);
    for (size_t i = 0; i < 8192; ++i) input[i] = produced++;
    assert(control_spsc_send((uint8_t *)input, 32768, &us) == 32768);
    assert(control_spsc_send((uint8_t *)input, 4096, &us) == 0); /* Full ring. */
    for (unsigned cycle = 0; cycle < 256; ++cycle) {
        /* 3840 visits 128 distinct ring offsets, including two-copy reads;
         * counter rollover occurs on the initial full-buffer publication. */
        assert(control_spsc_receive((uint8_t *)output, block) == block);
        for (size_t i = 0; i < block / 4; ++i) assert(output[i] == expected++);
        for (size_t i = 0; i < 1024; ++i) input[i] = produced + (uint32_t)i;
        /* Keep the real producer's 4096-byte shape. At capacity the aligned
         * consumer makes room for 3840; the rejected 256-byte tail is not queued. */
        assert(control_spsc_send((uint8_t *)input, 4096, &us) == block);
        produced += (uint32_t)block / 4;
        assert(control_spsc_consumer_fill() == 32768);
    }
#if CONTROL_DMA_ALIGNED
    assert(32768 % block != 0);
    assert(control_spsc_wrap_reads() != 0 && control_spsc_wrap_writes() != 0);
#endif
    while (control_spsc_consumer_fill() != 0) {
        size_t n = control_spsc_receive((uint8_t *)output, block);
        assert(n > 0 && n <= block && n % 4 == 0);
        for (size_t i = 0; i < n / 4; ++i) assert(output[i] == expected++);
    }
    assert(expected == produced);

    /* Aligned partial reads must not touch the unwritten tail of output. */
    memset(output, 0x5a, sizeof(output));
    assert(control_spsc_send((uint8_t *)input, (uint32_t)block - 4, &us) == block - 4);
    assert(control_spsc_receive((uint8_t *)output, block) == block - 4);
    assert(memcmp(input, output, block - 4) == 0);
    assert(output[block / 4 - 1] == 0x5a5a5a5a);
    assert(control_spsc_send((uint8_t *)input, (uint32_t)block, &us) == block);
    assert(control_spsc_receive((uint8_t *)output, block + 3) == block);
    assert(memcmp(input, output, block) == 0);
    assert(control_spsc_send((uint8_t *)input, (uint32_t)block + 1, &us) == 0);

    /* Consumer-owned finite-prefix flush in output-sized reads, across both
     * ring wrap and uint32 rollover. PCM published after cutoff remains queued. */
    seed(UINT32_MAX - 15U);
    const size_t cutoff = 3 * block + 4;
    assert(control_spsc_send((uint8_t *)input, (uint32_t)cutoff, &us) == cutoff);
    uint32_t fresh[1024];
    for (size_t i = 0; i < 1024; ++i) fresh[i] = (uint32_t)i ^ 0xabcdef00;
    assert(control_spsc_send((uint8_t *)fresh, sizeof(fresh), &us) == sizeof(fresh));
    const uint32_t published = __atomic_load_n(&s_write, __ATOMIC_RELAXED);
    size_t consumed = 0;
    while (consumed < cutoff) {
        const size_t remaining = cutoff - consumed;
        const size_t request = remaining < block ? remaining : block;
        assert(control_spsc_receive((uint8_t *)output, request) == request);
        consumed += request;
        assert(__atomic_load_n(&s_write, __ATOMIC_RELAXED) == published);
    }
    assert(control_spsc_consumer_fill() == sizeof(fresh));
    size_t offset = 0;
    while (offset < sizeof(fresh)) {
        size_t received = control_spsc_receive((uint8_t *)output, block);
        assert(received > 0 && received <= block && received % 4 == 0);
        assert(memcmp(output, (uint8_t *)fresh + offset, received) == 0);
        offset += received;
    }
    assert(control_spsc_consumer_fill() == 0);
}

static DWORD WINAPI paused_producer(void *unused)
{
    (void)unused;
    uint32_t us;
    assert(control_spsc_send((const uint8_t *)paused_source, 8, &us) == 8);
    return 0;
}

static void in_flight_flush_test(void)
{
    uint32_t us;
    seed(UINT32_MAX - 15U);
    assert(control_spsc_send((uint8_t *)input, 12, &us) == 12);
    copy_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    copy_continue = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(copy_entered && copy_continue);
    paused_source = input + 3;
    HANDLE thread = CreateThread(NULL, 0, paused_producer, NULL, 0, NULL);
    assert(thread && WaitForSingleObject(copy_entered, 30000) == WAIT_OBJECT_0);
    /* Producer copied part of the next old block, but has not published it.
     * The epoch snapshot's published cutoff includes only the first 12 bytes. */
    assert(control_spsc_consumer_fill() == 12);
    assert(control_spsc_receive((uint8_t *)output, 12) == 12);
    const uint32_t reader_after_discard = __atomic_load_n(&s_read, __ATOMIC_RELAXED);
    assert(SetEvent(copy_continue));
    assert(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
    assert(__atomic_load_n(&s_read, __ATOMIC_RELAXED) == reader_after_discard);
    paused_source = NULL;
    /* The next completed send fences the in-flight old producer. Drain its
     * published prefix; new data beyond a finite cutoff stays queued. */
    assert(control_spsc_consumer_fill() == 8);
    assert(control_spsc_send((uint8_t *)input + 20, 16, &us) == 16);
    assert(control_spsc_receive((uint8_t *)output, 8) == 8);
    assert(control_spsc_consumer_fill() == 16);
    assert(control_spsc_receive((uint8_t *)output, 16) == 16);
    assert(memcmp(output, (uint8_t *)input + 20, 16) == 0);
    CloseHandle(thread);
    CloseHandle(copy_entered);
    CloseHandle(copy_continue);
}

#define STRESS_FRAMES 8000000U
static DWORD WINAPI stress_producer(void *unused)
{
    (void)unused;
    uint32_t frames[1024], next = 0, us;
    while (next < STRESS_FRAMES) {
        uint32_t count = 1U + next % 1024U;
        if (count > STRESS_FRAMES - next) count = STRESS_FRAMES - next;
        for (uint32_t i = 0; i < count; ++i) frames[i] = next + i;
        const size_t sent = control_spsc_send((uint8_t *)frames, count * 4, &us);
        next += (uint32_t)sent / 4;
        assert(control_spsc_producer_fill() <= 32768);
        if (sent == 0) Sleep(0);
    }
    return 0;
}

static DWORD WINAPI stress_consumer(void *unused)
{
    (void)unused;
    uint32_t frames[CONTROL_CHUNK_BYTES / CONTROL_FRAME_BYTES], next = 0;
    while (next < STRESS_FRAMES) {
        const size_t received = control_spsc_receive((uint8_t *)frames, sizeof(frames));
        assert(received % 4 == 0);
        for (size_t i = 0; i < received / 4; ++i) assert(frames[i] == next++);
        assert(control_spsc_consumer_fill() <= 32768);
        if (received == 0) Sleep(0);
    }
    return 0;
}

int main(void)
{
    boundary_tests();
    fifo_oracle_test();
    output_block_tests();
    in_flight_flush_test();
    seed(UINT32_MAX - 16383U);
    HANDLE threads[] = {
        CreateThread(NULL, 0, stress_producer, NULL, 0, NULL),
        CreateThread(NULL, 0, stress_consumer, NULL, 0, NULL),
    };
    assert(threads[0] && threads[1]);
    assert(WaitForMultipleObjects(2, threads, TRUE, 30000) == WAIT_OBJECT_0);
    assert(control_spsc_consumer_fill() == 0);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    puts("PASS: boundary/alignment/full/partial, uint32 rollover, 100000 FIFO oracle steps,");
    puts("in-flight publication/discard, 8000000 ordered stereo frames across two threads.");
    printf("Output block=%u: wrap, partial reads, alignment, rollover, overflow and finite-prefix flush PASS.\n",
           CONTROL_CHUNK_BYTES);
    return 0;
}
