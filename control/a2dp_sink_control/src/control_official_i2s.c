/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Adapted from the installed ESP-IDF 6.1.0:
 * examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/
 * a2dp_sink_int_codec_utils/audio_sink_service_i2s.c and audio_sink_service.h.
 * State transitions and data path follow that source. See docs/ab-investigation.md
 * for the diagnostic and cooperative lifecycle deviations.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_clk_tree.h"
#include "control_variant.h"
#include "control_official_i2s.h"

#if CONTROL_VARIANT != 9
#error "Official I2S service belongs only to variant 9"
#endif
#ifndef CONTROL_OFFICIAL_ENV_NAME
#error "Select the official environment through PlatformIO/CMake"
#endif
_Static_assert(sizeof(uint32_t) == 4 && ATOMIC_INT_LOCK_FREE == 2,
               "Callback state/counters require lock-free 32-bit atomics");
_Static_assert(OFFICIAL_RING_BYTES == 32768 && OFFICIAL_PREFETCH_BYTES == 20480,
               "Keep official RingBuffer water levels");
_Static_assert(OFFICIAL_WRITE_UPTO == 1440 && OFFICIAL_WRITE_UPTO % 4 == 0,
               "Keep official receive maximum, in bytes");
_Static_assert(OFFICIAL_WRITER_PRIORITY == 22, "Unexpected FreeRTOS priorities");

static const char *TAG = "OFFICIAL";

typedef enum {
    PROCESSING,
    PREFETCHING,
    DROPPING,
} ring_mode_t;

typedef enum {
    CALLBACKS, PCM_BYTES, ACCEPTED_BYTES, DROPPED_PACKETS, DROPPED_BYTES,
    I2S_BYTES, WRITE_OPS, UNDERFLOWS, OVERFLOW_ENTRIES, I2S_ERRORS, SHORT_WRITES,
    COUNTER_COUNT,
} counter_t;

/* Each counter has ONE writer (PCM task or I2S task), and is never reset.
 * Relaxed load/store avoids RMW retry loops. Only stats owns 64-bit totals.
 */
static _Atomic uint32_t s_count[COUNTER_COUNT];
static _Atomic uint32_t s_mode = PREFETCHING;
static _Atomic uint32_t s_sample_rate = 44100;
static _Atomic uint32_t s_channels = 2;

/* These are lifetime flags, not transport locks. The callback never waits.
 * Sequential consistency pairs the active-before-gate reader protocol with
 * close's gate-before-active check, including calls racing with disconnect.
 * Only lifecycle code waits; stats uses its own flag and never locks PCM.
 */
static _Atomic uint32_t s_running;
static _Atomic uint32_t s_pcm_active;
static _Atomic uint32_t s_stats_active;
static _Atomic uint32_t s_exit_requested;
static i2s_chan_handle_t s_tx;
static bool s_enabled;
static RingbufHandle_t s_ring;
static SemaphoreHandle_t s_prefetch;
static SemaphoreHandle_t s_writer_exited;
static TaskHandle_t s_writer;

static void count_add(counter_t counter, uint32_t value)
{
    atomic_store_explicit(&s_count[counter],
        atomic_load_explicit(&s_count[counter], memory_order_relaxed) + value,
        memory_order_relaxed);
}

/* Official audio_sink_srv_data_output, with hot-path logs removed. In
 * DROPPING even the packet that releases drop mode is discarded, as upstream.
 * SDK RingBuffer/semaphore APIs retain their own internal synchronization.
 */
static size_t official_data_output(const uint8_t *data, size_t size)
{
    size_t item_size = 0;
    if (atomic_load_explicit(&s_mode, memory_order_relaxed) == DROPPING) {
        vRingbufferGetInfo(s_ring, NULL, NULL, NULL, NULL, &item_size);
        if (item_size <= OFFICIAL_PREFETCH_BYTES) {
            atomic_store_explicit(&s_mode, PROCESSING, memory_order_relaxed);
        }
        return 0;
    }

    BaseType_t done = xRingbufferSend(s_ring, (void *)data, size, (TickType_t)0);
    if (!done) {
        atomic_store_explicit(&s_mode, DROPPING, memory_order_relaxed);
        count_add(OVERFLOW_ENTRIES, 1);
    }
    if (atomic_load_explicit(&s_mode, memory_order_relaxed) == PREFETCHING) {
        vRingbufferGetInfo(s_ring, NULL, NULL, NULL, NULL, &item_size);
        if (item_size >= OFFICIAL_PREFETCH_BYTES) {
            atomic_store_explicit(&s_mode, PROCESSING, memory_order_relaxed);
            /* A full binary semaphore already represents the required wake. */
            (void)xSemaphoreGive(s_prefetch);
        }
    }
    return done ? size : 0;
}

void control_official_i2s_output(const uint8_t *data, uint32_t len)
{
    count_add(CALLBACKS, 1);
    count_add(PCM_BYTES, len);
    atomic_store_explicit(&s_pcm_active, 1, memory_order_seq_cst);
    size_t accepted = 0;
    if (atomic_load_explicit(&s_running, memory_order_seq_cst)) {
        accepted = official_data_output(data, len);
    }
    atomic_store_explicit(&s_pcm_active, 0, memory_order_seq_cst);
    count_add(ACCEPTED_BYTES, (uint32_t)accepted);
    if (accepted != len) {
        count_add(DROPPED_PACKETS, 1);
        count_add(DROPPED_BYTES, len);
    }
}

static void official_writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (pdTRUE == xSemaphoreTake(s_prefetch, portMAX_DELAY)) {
            if (atomic_load_explicit(&s_exit_requested, memory_order_acquire)) break;
            for (;;) {
                size_t item_size = 0;
                uint8_t *data = xRingbufferReceiveUpTo(s_ring, &item_size,
                    pdMS_TO_TICKS(OFFICIAL_READ_TIMEOUT_MS), OFFICIAL_WRITE_UPTO);
                if (item_size == 0) {
                    atomic_store_explicit(&s_mode, PREFETCHING, memory_order_relaxed);
                    count_add(UNDERFLOWS, 1);
                    break;
                }
                /* Close keeps the channel enabled until this task acknowledges
                 * completion. Never delete a task while it owns a ring item or
                 * is inside the driver's blocking write/semaphore operation.
                 */
                size_t written = 0;
                esp_err_t err = i2s_channel_write(s_tx, data, item_size,
                                                 &written, portMAX_DELAY);
                count_add(WRITE_OPS, 1);
                count_add(I2S_BYTES, (uint32_t)written);
                if (err != ESP_OK) count_add(I2S_ERRORS, 1);
                if (written != item_size) count_add(SHORT_WRITES, 1);
                /* Official behavior: one write, no retry of an unwritten tail. */
                vRingbufferReturnItem(s_ring, data);
                if (atomic_load_explicit(&s_exit_requested, memory_order_acquire)) break;
            }
            if (atomic_load_explicit(&s_exit_requested, memory_order_acquire)) break;
        }
    }
    /* All resource accesses are finished. The lifecycle owner deletes us. */
    xSemaphoreGive(s_writer_exited);
    for (;;) vTaskSuspend(NULL);
}

static void official_stats_task(void *arg)
{
    (void)arg;
    uint32_t previous[COUNTER_COUNT] = {0};
    uint64_t total[COUNTER_COUNT] = {0};
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(OFFICIAL_STATS_PERIOD_MS));
        int64_t now_us = esp_timer_get_time();
        uint32_t delta[COUNTER_COUNT];
        for (unsigned i = 0; i < COUNTER_COUNT; ++i) {
            uint32_t current = atomic_load_explicit(&s_count[i], memory_order_relaxed);
            delta[i] = current - previous[i];
            total[i] += delta[i];
            previous[i] = current;
        }
        size_t fill = 0;
        atomic_store_explicit(&s_stats_active, 1, memory_order_seq_cst);
        uint32_t running = atomic_load_explicit(&s_running, memory_order_seq_cst);
        if (running) vRingbufferGetInfo(s_ring, NULL, NULL, NULL, NULL, &fill);
        atomic_store_explicit(&s_stats_active, 0, memory_order_seq_cst);
        uint32_t rate = atomic_load_explicit(&s_sample_rate, memory_order_relaxed);
        uint32_t channels = atomic_load_explicit(&s_channels, memory_order_relaxed);
        int64_t elapsed = now_us - last_us;
        last_us = now_us;
        ESP_LOGI(TAG, "OFFICIAL stats: cb=%" PRIu64 ", pcm_total=%" PRIu64
                 ", pcm_interval=%" PRIu32 ", pcm_rate=%" PRIu64 " B/s"
                 ", accepted=%" PRIu64 ", dropped_packets=%" PRIu64
                 ", dropped_bytes=%" PRIu64 ", ring=%u/%u"
                 ", underflows=%" PRIu64 ", overflow_entries=%" PRIu64
                 ", i2s_written=%" PRIu64 ", i2s_rate=%" PRIu64 " B/s"
                 ", writes=%" PRIu64 ", err=%" PRIu64 ", short=%" PRIu64
                 ", expected=%" PRIu32 " B/s, sample_rate=%" PRIu32
                 ", channels=%" PRIu32 ", running=%" PRIu32
                 ", mode=%" PRIu32 ", elapsed_ms=%" PRId64,
                 total[CALLBACKS], total[PCM_BYTES], delta[PCM_BYTES],
                 (uint64_t)delta[PCM_BYTES] * 1000000 / elapsed,
                 total[ACCEPTED_BYTES], total[DROPPED_PACKETS], total[DROPPED_BYTES],
                 (unsigned)fill, OFFICIAL_RING_BYTES, total[UNDERFLOWS],
                 total[OVERFLOW_ENTRIES], total[I2S_BYTES],
                 (uint64_t)delta[I2S_BYTES] * 1000000 / elapsed, total[WRITE_OPS],
                 total[I2S_ERRORS], total[SHORT_WRITES], rate * channels * 2,
                 rate, channels, running,
                 atomic_load_explicit(&s_mode, memory_order_relaxed), elapsed / 1000);
    }
}

esp_err_t control_official_i2s_init(void)
{
    ESP_LOGI(TAG, "CONTROL env=%s variant=%s", CONTROL_OFFICIAL_ENV_NAME, CONTROL_VARIANT_NAME);
#if defined(CONFIG_BTDM_CTRL_MODEM_SLEEP) && CONFIG_BTDM_CTRL_MODEM_SLEEP
    const char *sleep_config = "enabled";
#else
    const char *sleep_config = "disabled";
#endif
    ESP_LOGI(TAG, "CONTROL cpu_config_mhz=%u bt_modem_sleep_config=%s",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, sleep_config);
    uint32_t cpu_hz = 0;
    const esp_err_t clock_result = esp_clk_tree_src_get_freq_hz(
        SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &cpu_hz);
    ESP_LOGI(TAG, "CONTROL cpu_runtime_hz=%" PRIu32 " clock_read_result=%d"
             " source=esp_clk_tree_src_get_freq_hz", cpu_hz, clock_result);
    /* Print fields of the same SDK initializer used by open; no DMA overrides. */
    const i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_LOGI(TAG, "CONTROL source=ESP-IDF-6.1 a2dp_sink_stream");
    ESP_LOGI(TAG, "CONTROL ring=%u prefetch=%u", OFFICIAL_RING_BYTES, OFFICIAL_PREFETCH_BYTES);
    ESP_LOGI(TAG, "CONTROL i2s=dma_desc_num=%u dma_frame_num=%u write_upto=%u"
             " task_priority=%u task_core=unpinned intr_priority=%d task_stack=%u",
             (unsigned)cfg.dma_desc_num, (unsigned)cfg.dma_frame_num,
             OFFICIAL_WRITE_UPTO, OFFICIAL_WRITER_PRIORITY, cfg.intr_priority,
             OFFICIAL_WRITER_STACK);
    ESP_LOGI(TAG, "CONTROL output_format=MSB official reference; no audible validation");
    ESP_LOGI(TAG, "CONTROL bck=%u ws=%u data=%u"
             " mclk=unused auto_clear=yes read_timeout_ms=%u stats_priority=1",
             OFFICIAL_BCK, OFFICIAL_WS, OFFICIAL_DATA, OFFICIAL_READ_TIMEOUT_MS);
    return xTaskCreate(official_stats_task, "official_stats", 4096, NULL, 1, NULL)
           == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t control_official_i2s_open(void)
{
    if (s_tx) return ESP_OK;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = OFFICIAL_BCK, .ws = OFFICIAL_WS,
            .dout = OFFICIAL_DATA, .din = I2S_GPIO_UNUSED,
        },
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) return err;
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_ERROR_CHECK(i2s_del_channel(s_tx));
        s_tx = NULL;
        return err;
    }
    atomic_store_explicit(&s_sample_rate, 44100, memory_order_relaxed);
    atomic_store_explicit(&s_channels, 2, memory_order_relaxed);
    ESP_LOGI(TAG, "service opened");
    return ESP_OK;
}

esp_err_t control_official_i2s_start(void)
{
    if (s_enabled) return ESP_OK;
    if (!s_tx) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) return err;
    s_enabled = true;
    atomic_store_explicit(&s_mode, PREFETCHING, memory_order_relaxed);
    atomic_store_explicit(&s_exit_requested, 0, memory_order_release);
    s_prefetch = xSemaphoreCreateBinary();
    s_writer_exited = xSemaphoreCreateBinary();
    s_ring = xRingbufferCreate(OFFICIAL_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_prefetch || !s_writer_exited || !s_ring ||
        xTaskCreate(official_writer_task, "BtI2STask", OFFICIAL_WRITER_STACK, NULL,
                    OFFICIAL_WRITER_PRIORITY, &s_writer) != pdPASS) {
        control_official_i2s_stop();
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_running, 1, memory_order_seq_cst);
    ESP_LOGI(TAG, "service started: PREFETCHING");
    return ESP_OK;
}

void control_official_i2s_stop(void)
{
    atomic_store_explicit(&s_running, 0, memory_order_seq_cst);
    /* Lifecycle only: never in the PCM callback or writer loop. */
    while (atomic_load_explicit(&s_pcm_active, memory_order_seq_cst) ||
           atomic_load_explicit(&s_stats_active, memory_order_seq_cst)) {
        vTaskDelay(1);
    }
    if (s_writer) {
        atomic_store_explicit(&s_exit_requested, 1, memory_order_release);
        (void)xSemaphoreGive(s_prefetch);
        xSemaphoreTake(s_writer_exited, portMAX_DELAY);
        vTaskDelete(s_writer);
        s_writer = NULL;
    }
    if (s_enabled) {
        ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
        s_enabled = false;
    }
    if (s_ring) { vRingbufferDelete(s_ring); s_ring = NULL; }
    if (s_prefetch) { vSemaphoreDelete(s_prefetch); s_prefetch = NULL; }
    if (s_writer_exited) { vSemaphoreDelete(s_writer_exited); s_writer_exited = NULL; }
}

void control_official_i2s_close(void)
{
    control_official_i2s_stop();
    if (s_tx) {
        ESP_ERROR_CHECK(i2s_del_channel(s_tx));
        s_tx = NULL;
        ESP_LOGI(TAG, "service closed: writer/ring/semaphores/channel released");
    }
}

esp_err_t control_official_i2s_codec_update(const esp_a2d_mcc_t *mcc)
{
    if (mcc->type != ESP_A2D_MCT_SBC) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = control_official_i2s_open();
    if (err != ESP_OK) return err;
    bool restart = s_enabled;
    control_official_i2s_stop();
    /* Same frequency precedence and mono/stereo mapping as official helper. */
    uint32_t sample_rate = 16000;
    uint32_t channels = 2;
    if (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_32K) sample_rate = 32000;
    else if (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_44K) sample_rate = 44100;
    else if (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K) sample_rate = 48000;
    if (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) channels = 1;
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
    err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
    if (err != ESP_OK) return err;
    err = i2s_channel_reconfig_std_slot(s_tx, &slot_cfg);
    if (err != ESP_OK) return err;
    atomic_store_explicit(&s_sample_rate, sample_rate, memory_order_relaxed);
    atomic_store_explicit(&s_channels, channels, memory_order_relaxed);
    ESP_LOGI(TAG, "SBC configured: sample_rate=%" PRIu32 ", channels=%" PRIu32
             ", expected=%" PRIu32 " B/s", sample_rate, channels, sample_rate * channels * 2);
    /* Initial CFG precedes CONNECTED. If a peer reconfigures an existing
     * connection, restart with a fresh prefetch instead of leaving I2S stopped.
     */
    return restart ? control_official_i2s_start() : ESP_OK;
}
