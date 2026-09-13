#include "i2s_output.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "app_diagnostics.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = APP_LOG_TAG;

#if !AUDIO_DIAGNOSTIC_BYPASS_I2S
static i2s_chan_handle_t tx_channel;
static uint32_t current_sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;

typedef struct
{
    TaskHandle_t waiter;
    esp_err_t result;
} i2s_init_context_t;

static esp_err_t i2s_output_init_channel(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.dma_desc_num = I2S_DMA_DESCRIPTOR_COUNT;
    channel_config.dma_frame_num = I2S_DMA_FRAMES_PER_DESCRIPTOR;
    const uint32_t dma_depth_us =
        I2S_DMA_DESCRIPTOR_COUNT * I2S_DMA_FRAMES_PER_DESCRIPTOR *
        1000000U / DEFAULT_SAMPLE_RATE_HZ;

    ESP_LOGI(TAG,
             "I2S init: port=%d, role=master, BCK=GPIO%d, LRCK/WS=GPIO%d, "
             "DATA=GPIO%d",
             I2S_PORT,
             I2S_BCK_GPIO,
             I2S_LRCK_GPIO,
             I2S_DATA_GPIO);
    ESP_LOGI(TAG,
             "I2S format: Philips/I2S, signed PCM s16le, interleaved L/R, "
             "bit_width=16, stereo, "
             "sample_rate=%lu Hz, DMA auto_clear=yes, descriptors=%u, "
             "frames/descriptor=%u, DMA depth=%lu.%03lu ms",
             (unsigned long)DEFAULT_SAMPLE_RATE_HZ,
             (unsigned)I2S_DMA_DESCRIPTOR_COUNT,
             (unsigned)I2S_DMA_FRAMES_PER_DESCRIPTOR,
             (unsigned long)(dma_depth_us / 1000U),
             (unsigned long)(dma_depth_us % 1000U));

    esp_err_t err = i2s_new_channel(&channel_config, &tx_channel, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_GPIO,
            .ws = I2S_LRCK_GPIO,
            .dout = I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_channel, &standard_config);
    if (err != ESP_OK)
    {
        return err;
    }

#if AUDIO_DIAGNOSTIC_DRAIN_PCM
    static const uint8_t silence[AUDIO_WRITE_BUFFER_SIZE] = {0};
    size_t total_preloaded = 0;
    while (true)
    {
        size_t bytes_loaded = 0;
        err = i2s_channel_preload_data(
            tx_channel,
            silence,
            sizeof(silence),
            &bytes_loaded);
        if (err != ESP_OK)
        {
            return err;
        }
        total_preloaded += bytes_loaded;
        if (bytes_loaded < sizeof(silence))
        {
            break;
        }
    }
    ESP_LOGI(TAG,
             "I2S diagnostic silence preloaded: %u bytes across DMA "
             "descriptors",
             (unsigned)total_preloaded);
#endif

    err = i2s_channel_enable(tx_channel);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "I2S port %d initialized and enabled", I2S_PORT);
    }
    return err;
}

static void i2s_init_task(void *argument)
{
    i2s_init_context_t *context = (i2s_init_context_t *)argument;

    ESP_LOGI(TAG,
             "I2S initialization task running on core %d; DMA interrupt "
             "will be allocated on this core",
             (int)xPortGetCoreID());
    context->result = i2s_output_init_channel();
    app_diagnostics_record_stack(DIAG_TASK_I2S_INIT);
    xTaskNotifyGive(context->waiter);
    vTaskDelete(NULL);
}
#endif

esp_err_t i2s_output_init(void)
{
#if AUDIO_DIAGNOSTIC_BYPASS_I2S
    ESP_LOGW(TAG,
             "DIAGNOSTIC BUILD: I2S/DMA disabled; GPIO BCK=%d, LRCK=%d, "
             "DATA=%d will not be configured and there will be no audio "
             "output",
             I2S_BCK_GPIO,
             I2S_LRCK_GPIO,
             I2S_DATA_GPIO);
    return ESP_OK;
#else
    i2s_init_context_t context = {
        .waiter = xTaskGetCurrentTaskHandle(),
        .result = ESP_FAIL,
    };
    BaseType_t result = xTaskCreatePinnedToCore(
        i2s_init_task,
        "i2s_init",
        I2S_INIT_TASK_STACK_SIZE,
        &context,
        AUDIO_WRITER_TASK_PRIORITY,
        NULL,
        AUDIO_WRITER_TASK_CORE);
    if (result != pdPASS)
    {
        ESP_LOGE(TAG,
                 "Cannot create I2S initialization task on core %d",
                 AUDIO_WRITER_TASK_CORE);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Task created: i2s_init, priority=%d, core=%d",
             AUDIO_WRITER_TASK_PRIORITY,
             AUDIO_WRITER_TASK_CORE);

    if (ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(I2S_INIT_TIMEOUT_MS)) == 0)
    {
        ESP_LOGE(TAG,
                 "I2S initialization on core %d timed out after %u ms",
                 AUDIO_WRITER_TASK_CORE,
                 (unsigned)I2S_INIT_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    if (context.result != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "I2S initialization failed: %s",
                 esp_err_to_name(context.result));
        return context.result;
    }

    ESP_LOGI(TAG,
             "I2S initialization completed on core %d before Bluetooth start",
             AUDIO_WRITER_TASK_CORE);
#if AUDIO_DIAGNOSTIC_DRAIN_PCM
    ESP_LOGW(TAG,
             "DIAGNOSTIC BUILD: I2S clocks and DMA enabled with silent "
             "buffers; decoded Bluetooth PCM will not be written to I2S");
#endif
    return ESP_OK;
#endif
}

esp_err_t i2s_output_set_sample_rate(uint32_t sample_rate_hz)
{
#if AUDIO_DIAGNOSTIC_BYPASS_I2S || AUDIO_DIAGNOSTIC_DRAIN_PCM
    (void)sample_rate_hz;
    return ESP_OK;
#else
    if (sample_rate_hz == current_sample_rate_hz)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "Changing I2S sample rate: %lu -> %lu Hz",
             (unsigned long)current_sample_rate_hz,
             (unsigned long)sample_rate_hz);

    esp_err_t err = i2s_channel_disable(tx_channel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Cannot disable I2S port %d for rate change: %s",
                 I2S_PORT,
                 esp_err_to_name(err));
        return err;
    }

    i2s_std_clk_config_t clock_config =
        I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    err = i2s_channel_reconfig_std_clock(tx_channel, &clock_config);
    if (err == ESP_OK)
    {
        current_sample_rate_hz = sample_rate_hz;
    }

    esp_err_t enable_err = i2s_channel_enable(tx_channel);
    if (err == ESP_OK)
    {
        err = enable_err;
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "I2S sample rate: %lu Hz", (unsigned long)sample_rate_hz);
    }
    else
    {
        ESP_LOGE(TAG,
                 "I2S sample-rate change to %lu Hz failed: %s",
                 (unsigned long)sample_rate_hz,
                 esp_err_to_name(err));
    }
    return err;
#endif
}

esp_err_t i2s_output_write(
    const void *data,
    size_t size,
    size_t *bytes_written)
{
    if (bytes_written == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes_written = 0;
    if (data == NULL || size == 0 || size % AUDIO_PCM_FRAME_BYTES != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
#if AUDIO_DIAGNOSTIC_BYPASS_I2S
    (void)data;
    (void)size;
    if (bytes_written != NULL)
    {
        *bytes_written = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* This API takes milliseconds, not FreeRTOS ticks. DMA paces the writer. */
    return i2s_channel_write(
        tx_channel, data, size, bytes_written, I2S_WRITE_TIMEOUT_MS);
#endif
}
