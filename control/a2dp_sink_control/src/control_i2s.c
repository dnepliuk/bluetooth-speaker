/* Minimal local adaptation of main src/i2s_output.c, ESP-IDF 6.1 API.
 * The persistent writer also initializes I2S so its interrupt belongs to core 1.
 */
#include "control_i2s.h"
#include "control_audio_config.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

static i2s_chan_handle_t s_tx;
static uint32_t s_rate = CONTROL_SAMPLE_RATE;

esp_err_t control_i2s_init(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = CONTROL_DMA_DESCRIPTORS;
    channel.dma_frame_num = CONTROL_DMA_FRAMES;
    esp_err_t err = i2s_new_channel(&channel, &s_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONTROL_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_26,
            .ws = GPIO_NUM_25,
            .dout = GPIO_NUM_22,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &config);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(s_tx);
}

esp_err_t control_i2s_set_rate(uint32_t sample_rate)
{
    if (sample_rate == s_rate) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    const i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    err = i2s_channel_reconfig_std_clock(s_tx, &clock);
    if (err == ESP_OK) {
        s_rate = sample_rate;
    }
    esp_err_t enabled = i2s_channel_enable(s_tx);
    return err == ESP_OK ? enabled : err;
}

esp_err_t control_i2s_write(const uint8_t *data, size_t size, size_t *written)
{
    /* IDF takes milliseconds here, not FreeRTOS ticks. */
    return i2s_channel_write(s_tx, data, size, written, CONTROL_WRITE_TIMEOUT_MS);
}
