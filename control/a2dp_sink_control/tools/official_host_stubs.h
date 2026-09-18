/* Test-only Windows adapters, not an ESP32 scheduler or DMA simulation. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <windows.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERROR_CHECK(expr) assert((expr) == ESP_OK)
void host_log(const char *, const char *, ...);
#define ESP_LOGI host_log

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef HANDLE TaskHandle_t;
typedef HANDLE SemaphoreHandle_t;
typedef struct host_ring *RingbufHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define configMAX_PRIORITIES 25
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
#define CONFIG_BTDM_CTRL_MODEM_SLEEP 1
#define RINGBUF_TYPE_BYTEBUF 2
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void vTaskSuspend(TaskHandle_t);
void vTaskDelay(TickType_t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
RingbufHandle_t xRingbufferCreate(size_t, int);
void vRingbufferDelete(RingbufHandle_t);
BaseType_t xRingbufferSend(RingbufHandle_t, void *, size_t, TickType_t);
void *xRingbufferReceiveUpTo(RingbufHandle_t, size_t *, TickType_t, size_t);
void vRingbufferReturnItem(RingbufHandle_t, void *);
void vRingbufferGetInfo(RingbufHandle_t, void *, void *, void *, void *, size_t *);
int64_t esp_timer_get_time(void);
#define SOC_MOD_CLK_CPU 1
#define ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT 2
esp_err_t esp_clk_tree_src_get_freq_hz(int, int, uint32_t *);

typedef struct host_channel *i2s_chan_handle_t;
typedef struct { unsigned id, role, dma_desc_num, dma_frame_num; bool auto_clear; int intr_priority; } i2s_chan_config_t;
typedef struct { unsigned sample_rate_hz; } i2s_std_clk_config_t;
typedef struct { unsigned bits, slot_mode; bool msb; } i2s_std_slot_config_t;
typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_GPIO_UNUSED (-1)
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(id_, role_) { .id=id_, .role=role_, .dma_desc_num=6, .dma_frame_num=240 }
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { .sample_rate_hz=rate }
#define I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits_, channels) { .bits=bits_, .slot_mode=channels, .msb=true }
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, void *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, uint32_t);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t, const i2s_std_clk_config_t *);
esp_err_t i2s_channel_reconfig_std_slot(i2s_chan_handle_t, const i2s_std_slot_config_t *);

#define ESP_A2D_MCT_SBC 0
#define ESP_A2D_SBC_CIE_SF_32K 4
#define ESP_A2D_SBC_CIE_SF_44K 2
#define ESP_A2D_SBC_CIE_SF_48K 1
#define ESP_A2D_SBC_CIE_CH_MODE_MONO 8
typedef struct {
    int type;
    union { struct { uint8_t samp_freq, ch_mode; } sbc_info; } cie;
} esp_a2d_mcc_t;
