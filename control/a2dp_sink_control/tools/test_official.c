/* Production service exercised through bounded Windows API adapters. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "official_host_stubs.h"
#include "../src/control_official_i2s.c"

#ifdef NDEBUG
#error "These tests require assertions"
#endif

struct host_ring {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE available;
    size_t read, write, used, held, capacity;
    uint8_t bytes[OFFICIAL_RING_BYTES];
};
struct host_channel { bool enabled; unsigned rate, channels; };
static int live_rings, live_semaphores, live_tasks, live_channels;
static int fail_at, allocation;
static HANDLE writer_gate, write_gate, write_entered, send_gate, send_entered;
static _Atomic uint32_t write_calls, bytes_out, short_next, error_next, send_blocked;
static uint8_t captured[256 * 1024];
static size_t requests[1024];
static uint8_t packet[OFFICIAL_RING_BYTES];

static bool fail(void) { return ++allocation == fail_at; }
static void join(HANDLE thread)
{
    assert(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
    CloseHandle(thread);
}
static void await_at_least(_Atomic uint32_t *value, uint32_t target)
{
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (atomic_load(value) < target) { assert(GetTickCount64() < deadline); Sleep(1); }
}
static void reset_capture(void)
{
    atomic_store(&write_calls, 0); atomic_store(&bytes_out, 0);
    atomic_store(&short_next, 0); atomic_store(&error_next, 0);
    ResetEvent(write_entered); SetEvent(write_gate);
}

void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return (int64_t)GetTickCount64() * 1000; }
esp_err_t esp_clk_tree_src_get_freq_hz(int source, int precision, uint32_t *hz)
{
    assert(source == SOC_MOD_CLK_CPU && precision == ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT);
    *hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000U;
    return ESP_OK;
}
void vTaskDelay(TickType_t ticks) { Sleep(ticks); }
void vTaskSuspend(TaskHandle_t handle) { assert(!handle); ExitThread(0); }
typedef struct { void (*function)(void *); void *argument; } task_args;
static DWORD WINAPI task_entry(void *arg)
{
    task_args copy = *(task_args *)arg; free(arg);
    assert(WaitForSingleObject(writer_gate, 3000) == WAIT_OBJECT_0);
    copy.function(copy.argument);
    return 0;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *handle)
{
    if (!strcmp(name, "official_stats")) {
        assert(stack == 4096 && priority == 1 && !handle);
        return pdPASS; /* The tests drive lifetime sampling directly. */
    }
    assert(stack == 4096 && priority == 22 && handle);
    if (fail()) return pdFALSE;
    task_args *a = malloc(sizeof(*a)); assert(a); *a = (task_args){fn, arg};
    *handle = CreateThread(NULL, 0, task_entry, a, 0, NULL); assert(*handle);
    ++live_tasks; return pdPASS;
}
void vTaskDelete(TaskHandle_t handle) { join(handle); --live_tasks; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if (fail()) return NULL;
    HANDLE h = CreateSemaphore(NULL, 0, 1, NULL); assert(h); ++live_semaphores; return h;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t timeout)
{
    return WaitForSingleObject(h, timeout == portMAX_DELAY ? 3000 : timeout) == WAIT_OBJECT_0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { return ReleaseSemaphore(h, 1, NULL) != 0; }
void vSemaphoreDelete(SemaphoreHandle_t h) { assert(CloseHandle(h)); --live_semaphores; }

RingbufHandle_t xRingbufferCreate(size_t size, int type)
{
    assert(size == OFFICIAL_RING_BYTES && type == RINGBUF_TYPE_BYTEBUF);
    if (fail()) return NULL;
    RingbufHandle_t r = calloc(1, sizeof(*r)); assert(r);
    r->capacity = size; InitializeCriticalSection(&r->lock);
    InitializeConditionVariable(&r->available); ++live_rings; return r;
}
void vRingbufferDelete(RingbufHandle_t r)
{
    assert(!r->held); DeleteCriticalSection(&r->lock); free(r); --live_rings;
}
BaseType_t xRingbufferSend(RingbufHandle_t r, void *data, size_t size, TickType_t timeout)
{
    assert(timeout == 0);
    if (atomic_load(&send_blocked)) {
        SetEvent(send_entered); assert(WaitForSingleObject(send_gate, 3000) == WAIT_OBJECT_0);
    }
    EnterCriticalSection(&r->lock);
    if (size > r->capacity - r->used) { LeaveCriticalSection(&r->lock); return pdFALSE; }
    size_t first = size < r->capacity - r->write ? size : r->capacity - r->write;
    memcpy(r->bytes + r->write, data, first);
    memcpy(r->bytes, (uint8_t *)data + first, size - first);
    r->write = (r->write + size) % r->capacity; r->used += size;
    WakeConditionVariable(&r->available); LeaveCriticalSection(&r->lock); return pdTRUE;
}
void *xRingbufferReceiveUpTo(RingbufHandle_t r, size_t *size, TickType_t timeout, size_t maximum)
{
    EnterCriticalSection(&r->lock); assert(!r->held);
    if (!r->used && timeout) SleepConditionVariableCS(&r->available, &r->lock, timeout);
    *size = r->used < maximum ? r->used : maximum;
    if (*size > r->capacity - r->read) *size = r->capacity - r->read;
    r->held = *size;
    void *data = *size ? r->bytes + r->read : NULL;
    LeaveCriticalSection(&r->lock); return data;
}
void vRingbufferReturnItem(RingbufHandle_t r, void *data)
{
    EnterCriticalSection(&r->lock);
    assert(r->held && data == r->bytes + r->read);
    r->read = (r->read + r->held) % r->capacity; r->used -= r->held; r->held = 0;
    LeaveCriticalSection(&r->lock);
}
void vRingbufferGetInfo(RingbufHandle_t r, void *a, void *b, void *c, void *d, size_t *size)
{
    assert(!a && !b && !c && !d);
    EnterCriticalSection(&r->lock); *size = r->used - r->held; LeaveCriticalSection(&r->lock);
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *out, void *rx)
{
    assert(!rx && cfg->dma_desc_num == 6 && cfg->dma_frame_num == 240 && cfg->auto_clear);
    assert(cfg->intr_priority == 0);
    if (fail()) return ESP_ERR_NO_MEM;
    *out = calloc(1, sizeof(**out)); assert(*out); ++live_channels; return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t c, const i2s_std_config_t *cfg)
{
    assert(cfg->slot_cfg.msb && cfg->slot_cfg.bits == 16);
    assert(cfg->gpio_cfg.bclk == 26 && cfg->gpio_cfg.ws == 25 && cfg->gpio_cfg.dout == 22);
    assert(cfg->gpio_cfg.mclk == -1 && cfg->gpio_cfg.din == -1);
    if (fail()) return ESP_ERR_NO_MEM;
    c->rate = cfg->clk_cfg.sample_rate_hz; c->channels = cfg->slot_cfg.slot_mode; return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t c)
{
    assert(!c->enabled); if (fail()) return ESP_ERR_NO_MEM;
    c->enabled = true; return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t c)
{
    assert(c->enabled && !live_tasks); c->enabled = false; return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t c)
{
    assert(!c->enabled); free(c); --live_channels; return ESP_OK;
}
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t c, const i2s_std_clk_config_t *cfg)
{
    assert(!c->enabled); c->rate = cfg->sample_rate_hz; return ESP_OK;
}
esp_err_t i2s_channel_reconfig_std_slot(i2s_chan_handle_t c, const i2s_std_slot_config_t *cfg)
{
    assert(!c->enabled && cfg->msb); c->channels = cfg->slot_mode; return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t c, const void *data, size_t size, size_t *written, uint32_t timeout)
{
    assert(c->enabled && size && size <= 1440 && timeout == portMAX_DELAY);
    SetEvent(write_entered); assert(WaitForSingleObject(write_gate, 3000) == WAIT_OBJECT_0);
    unsigned call = atomic_load(&write_calls), offset = atomic_load(&bytes_out);
    assert(call < 1024); requests[call] = size;
    *written = atomic_exchange(&short_next, 0) ? size / 2 : size;
    assert(offset + *written <= sizeof(captured)); memcpy(captured + offset, data, *written);
    atomic_store(&bytes_out, offset + *written); atomic_store(&write_calls, call + 1);
    return atomic_exchange(&error_next, 0) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void assert_closed(void)
{
    assert(!s_tx && !s_ring && !s_prefetch && !s_writer_exited && !s_writer);
    assert(!live_tasks && !live_channels && !live_rings && !live_semaphores);
}
static void begin(void)
{
    assert_closed(); reset_capture(); SetEvent(writer_gate);
    assert(control_official_i2s_open() == ESP_OK);
    assert(control_official_i2s_start() == ESP_OK);
    assert(atomic_load(&s_mode) == PREFETCHING);
}
static void close_service(void) { SetEvent(writer_gate); control_official_i2s_close(); assert_closed(); }
static void send_chunks(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) control_official_i2s_output(packet, 1024);
}
static DWORD WINAPI close_thread(void *unused) { (void)unused; control_official_i2s_close(); return 0; }
static DWORD WINAPI send_thread(void *unused) { (void)unused; control_official_i2s_output(packet, 1024); return 0; }

int main(void)
{
    writer_gate = CreateEvent(NULL, TRUE, TRUE, NULL);
    write_gate = CreateEvent(NULL, TRUE, TRUE, NULL); write_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    send_gate = CreateEvent(NULL, TRUE, FALSE, NULL); send_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    for (unsigned i = 0; i < sizeof(packet); ++i) packet[i] = (uint8_t)i;
    assert(control_official_i2s_init() == ESP_OK);

    /* Initial prefetch, exact PCM order, wrap, repeated prefetch, no synthetic PCM. */
    begin(); uint32_t underflows = atomic_load(&s_count[UNDERFLOWS]);
    send_chunks(19); Sleep(30); assert(atomic_load(&write_calls) == 0);
    send_chunks(1); await_at_least(&s_count[UNDERFLOWS], underflows + 1);
    assert(atomic_load(&bytes_out) == 20480 && atomic_load(&s_mode) == PREFETCHING);
    unsigned calls = atomic_load(&write_calls);
    send_chunks(19); Sleep(30); assert(atomic_load(&write_calls) == calls);
    send_chunks(1); await_at_least(&s_count[UNDERFLOWS], underflows + 2);
    assert(atomic_load(&bytes_out) == 40960);
    for (unsigned i = 0; i < 40960; ++i) assert(captured[i] == (uint8_t)i);
    close_service(); puts("PASS prefetch/re-prefetch, wrap/order, no zero padding");

    /* Full-packet admission and exact official DROPPING exit packet semantics. */
    ResetEvent(writer_gate);
    assert(control_official_i2s_open() == ESP_OK && control_official_i2s_start() == ESP_OK);
    uint32_t accepted = atomic_load(&s_count[ACCEPTED_BYTES]);
    uint32_t dropped = atomic_load(&s_count[DROPPED_PACKETS]);
    control_official_i2s_output(packet, 32704); control_official_i2s_output(packet, 128);
    assert(atomic_load(&s_count[ACCEPTED_BYTES]) - accepted == 32704);
    assert(s_ring->used == 32704 && atomic_load(&s_mode) == DROPPING);
    control_official_i2s_output(packet, 1024); assert(atomic_load(&s_mode) == DROPPING);
    size_t size; void *item = xRingbufferReceiveUpTo(s_ring, &size, 0, 12224);
    assert(size == 12224); vRingbufferReturnItem(s_ring, item);
    control_official_i2s_output(packet, 1024);
    assert(atomic_load(&s_mode) == PROCESSING && s_ring->used == 20480);
    assert(atomic_load(&s_count[DROPPED_PACKETS]) - dropped == 3);
    control_official_i2s_output(packet, 1024); assert(s_ring->used == 21504);
    close_service(); puts("PASS overflow is whole-packet; threshold exit packet also dropped");

    /* No retry: failed short first write loses its tail and is counted once. */
    begin(); atomic_store(&short_next, 1); atomic_store(&error_next, 1);
    uint32_t shorts = atomic_load(&s_count[SHORT_WRITES]), errors = atomic_load(&s_count[I2S_ERRORS]);
    underflows = atomic_load(&s_count[UNDERFLOWS]); send_chunks(20);
    await_at_least(&s_count[UNDERFLOWS], underflows + 1);
    assert(atomic_load(&write_calls) == 15 && requests[0] == 1440 && requests[14] == 320);
    assert(atomic_load(&bytes_out) == 20480 - 720);
    assert(atomic_load(&s_count[SHORT_WRITES]) == shorts + 1 && atomic_load(&s_count[I2S_ERRORS]) == errors + 1);
    close_service(); puts("PASS I2S error/short counters, no tail retry");

    /* SBC updates before connection, live reconfiguration, mono as upstream. */
    esp_a2d_mcc_t mcc = {.type=ESP_A2D_MCT_SBC, .cie.sbc_info={ESP_A2D_SBC_CIE_SF_48K, 1}};
    assert(control_official_i2s_codec_update(&mcc) == ESP_OK);
    assert(s_tx->rate == 48000 && !s_tx->enabled && s_tx->channels == 2);
    assert(control_official_i2s_start() == ESP_OK);
    mcc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    assert(control_official_i2s_codec_update(&mcc) == ESP_OK);
    assert(s_tx->rate == 44100 && s_tx->enabled && live_tasks == 1 && live_rings == 1);
    mcc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO;
    assert(control_official_i2s_codec_update(&mcc) == ESP_OK && s_tx->channels == 1);
    close_service(); puts("PASS 44.1/48 kHz, mono/stereo, live codec restart");

    /* Close must wait for a blocking write and return its outstanding item. */
    begin(); ResetEvent(write_gate); send_chunks(20);
    assert(WaitForSingleObject(write_entered, 3000) == WAIT_OBJECT_0);
    HANDLE closer = CreateThread(NULL, 0, close_thread, NULL, 0, NULL);
    assert(WaitForSingleObject(closer, 30) == WAIT_TIMEOUT);
    SetEvent(write_gate); join(closer); assert_closed();

    /* Close must also wait for a callback that entered the zero-timeout send. */
    begin(); atomic_store(&send_blocked, 1);
    HANDLE sender = CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    assert(WaitForSingleObject(send_entered, 3000) == WAIT_OBJECT_0);
    closer = CreateThread(NULL, 0, close_thread, NULL, 0, NULL);
    assert(WaitForSingleObject(closer, 30) == WAIT_TIMEOUT);
    SetEvent(send_gate); join(sender); join(closer); atomic_store(&send_blocked, 0); assert_closed();
    begin(); atomic_store(&s_stats_active, 1);
    closer = CreateThread(NULL, 0, close_thread, NULL, 0, NULL);
    assert(WaitForSingleObject(closer, 30) == WAIT_TIMEOUT);
    atomic_store(&s_stats_active, 0); join(closer); assert_closed();
    puts("PASS close waits for in-flight writer, callback and stats reader");

    /* Every open/start allocation and driver setup failure unwinds resources. */
    for (int failure = 1; failure <= 7; ++failure) {
        allocation = 0; fail_at = failure;
        esp_err_t err = control_official_i2s_open();
        if (err == ESP_OK) err = control_official_i2s_start();
        assert(err != ESP_OK); close_service();
    }
    fail_at = 0;
    for (unsigned i = 0; i < 32; ++i) { begin(); close_service(); }
    uint32_t drop_before = atomic_load(&s_count[DROPPED_PACKETS]);
    control_official_i2s_output(packet, 1024);
    assert(atomic_load(&s_count[DROPPED_PACKETS]) == drop_before + 1);
    puts("PASS partial initialization failures, 32 reconnects, output after close");
    CloseHandle(writer_gate); CloseHandle(write_gate); CloseHandle(write_entered);
    CloseHandle(send_gate); CloseHandle(send_entered);
    return 0;
}
