#pragma once

#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "freertos/FreeRTOS.h"

#define APP_LOG_TAG "FAITAL_BT"
#define BLUETOOTH_DEVICE_NAME "Faital Speaker Proto"

/* Keep all diagnostic flags at 0 for normal Bluetooth playback. */
#define AUDIO_DIAGNOSTIC_TONE_ENABLED 0
#define AUDIO_DIAGNOSTIC_DRAIN_PCM 0
#define AUDIO_DIAGNOSTIC_BYPASS_I2S 0

/* A/B diagnostics: change one variable at a time; keep both 0 normally. */
#define BT_DIAGNOSTIC_VERBOSE_STACK_LOGS 0
#define BT_DIAGNOSTIC_MINIMAL_MODE 1

#define DEFAULT_SAMPLE_RATE_HZ 44100U
#define AUDIO_STREAM_BUFFER_SIZE (32U * 1024U)
#define AUDIO_PREFETCH_THRESHOLD (8U * 1024U)
#define AUDIO_WRITE_BUFFER_SIZE 4096U
#define AUDIO_STATS_INTERVAL_MS 5000U
#define AUDIO_STATS_POLL_MS 100U
#define AUDIO_RECEIVE_TIMEOUT_MS 100U
#define AUDIO_PREFETCH_POLL_MS 10U
#define AUDIO_PCM_GAP_THRESHOLD_MS 35U
#define AUDIO_PCM_FRAME_BYTES 4U
/* Emergency recovery only: leave room for one normal 4096-byte callback. */
#define AUDIO_LATENCY_HIGH_WATER (AUDIO_STREAM_BUFFER_SIZE - AUDIO_WRITE_BUFFER_SIZE)
#define AUDIO_LATENCY_TARGET (8U * 1024U)
#define AUDIO_WRITER_TASK_PRIORITY (configMAX_PRIORITIES - 3)
#define AUDIO_WRITER_TASK_CORE 1
#define AUDIO_WRITER_TASK_STACK_SIZE 8192U
#define I2S_INIT_TASK_STACK_SIZE 4096U
#define I2S_INIT_TIMEOUT_MS 5000U
#define I2S_WRITE_TIMEOUT_MS 1000U
#define AUDIO_STATS_TASK_PRIORITY 1
#define BT_RSSI_TASK_PRIORITY 1
#define BT_RSSI_INITIAL_DELAY_MS 2500U
#define BT_RSSI_INTERVAL_MS 5000U

#define I2S_DMA_DESCRIPTOR_COUNT 3U
#define I2S_DMA_FRAMES_PER_DESCRIPTOR 960U

#define I2S_PORT I2S_NUM_0
#define I2S_BCK_GPIO GPIO_NUM_26
#define I2S_LRCK_GPIO GPIO_NUM_25
#define I2S_DATA_GPIO GPIO_NUM_22

#define STATUS_LED_GPIO GPIO_NUM_2
#define STATUS_LED_ACTIVE_LEVEL 1
#define STATUS_LED_TASK_PERIOD_MS 25U

#define NVS_NAMESPACE "bt_audio"
#define NVS_LAST_BDA_KEY "last_bda"

#if AUDIO_STATS_INTERVAL_MS < 5000U
#error "Audio/BT diagnostics must not log more often than every five seconds"
#endif

#if AUDIO_DIAGNOSTIC_TONE_ENABLED && \
    (AUDIO_DIAGNOSTIC_DRAIN_PCM || AUDIO_DIAGNOSTIC_BYPASS_I2S)
#error "Diagnostic tone requires I2S and cannot be combined with PCM drain"
#endif

#if AUDIO_DIAGNOSTIC_BYPASS_I2S && !AUDIO_DIAGNOSTIC_DRAIN_PCM
#error "I2S bypass requires PCM drain"
#endif

#if (AUDIO_STREAM_BUFFER_SIZE % AUDIO_PCM_FRAME_BYTES) != 0 || \
    (AUDIO_WRITE_BUFFER_SIZE % AUDIO_PCM_FRAME_BYTES) != 0 || \
    (AUDIO_PREFETCH_THRESHOLD % AUDIO_PCM_FRAME_BYTES) != 0
#error "Audio buffers must preserve complete stereo PCM frames"
#endif

#if I2S_DMA_FRAMES_PER_DESCRIPTOR * AUDIO_PCM_FRAME_BYTES > 4092U
#error "An ESP32 DMA descriptor must fit within 4092 aligned bytes"
#endif

#if AUDIO_PREFETCH_THRESHOLD >= AUDIO_STREAM_BUFFER_SIZE
#error "AUDIO_PREFETCH_THRESHOLD must be smaller than AUDIO_STREAM_BUFFER_SIZE"
#endif

#if AUDIO_LATENCY_TARGET >= AUDIO_LATENCY_HIGH_WATER
#error "AUDIO_LATENCY_TARGET must be smaller than AUDIO_LATENCY_HIGH_WATER"
#endif

#if AUDIO_LATENCY_HIGH_WATER >= AUDIO_STREAM_BUFFER_SIZE
#error "AUDIO_LATENCY_HIGH_WATER must be smaller than AUDIO_STREAM_BUFFER_SIZE"
#endif

#if (AUDIO_LATENCY_TARGET % AUDIO_PCM_FRAME_BYTES) != 0 || \
    (AUDIO_LATENCY_HIGH_WATER % AUDIO_PCM_FRAME_BYTES) != 0
#error "Audio latency limits must preserve complete stereo PCM frames"
#endif
