#include "app_config.h"
#include "audio_pipeline.h"
#include "bluetooth_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "status_led.h"

static const char *TAG = APP_LOG_TAG;

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG,
                 "NVS requires erase and reinitialization: %s",
                 esp_err_to_name(err));
        status_led_signal_error();
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized successfully");
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main started");

    ESP_ERROR_CHECK(status_led_init());
    initialize_nvs();
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(bluetooth_audio_init());

    ESP_LOGI(TAG, "Initialization complete");
}
