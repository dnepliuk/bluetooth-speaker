#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "app_diagnostics.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = APP_LOG_TAG;

static volatile status_led_state_t status_led_state = STATUS_LED_INITIALIZING;
static volatile bool status_led_error_requested;

static const char *status_led_state_name(status_led_state_t state)
{
    switch (state)
    {
    case STATUS_LED_INITIALIZING:
        return "initializing";
    case STATUS_LED_DISCOVERABLE:
        return "discoverable";
    case STATUS_LED_CONNECTED:
        return "connected";
    case STATUS_LED_STREAMING:
        return "streaming";
    case STATUS_LED_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

void status_led_set_state(status_led_state_t state)
{
    __atomic_store_n(&status_led_state, state, __ATOMIC_RELAXED);
}

void status_led_signal_error(void)
{
    __atomic_store_n(&status_led_error_requested, true, __ATOMIC_RELAXED);
}

static void status_led_write(bool on)
{
    gpio_set_level(
        STATUS_LED_GPIO,
        on ? STATUS_LED_ACTIVE_LEVEL : !STATUS_LED_ACTIVE_LEVEL);
}

static void status_led_task(void *argument)
{
    (void)argument;
    app_diagnostics_track_task(DIAG_TASK_LED);

    status_led_state_t previous_state = (status_led_state_t)-1;
    TickType_t pattern_started_at = xTaskGetTickCount();
    bool error_pattern_active = false;

    while (true)
    {
        TickType_t now = xTaskGetTickCount();
        status_led_state_t state =
            __atomic_load_n(&status_led_state, __ATOMIC_RELAXED);

        if (state != previous_state)
        {
            ESP_LOGI(TAG, "Status LED state: %s", status_led_state_name(state));
            previous_state = state;
            pattern_started_at = now;
        }

        if (__atomic_exchange_n(
                &status_led_error_requested,
                false,
                __ATOMIC_RELAXED))
        {
            error_pattern_active = true;
            pattern_started_at = now;
            ESP_LOGW(TAG, "Status LED: showing recoverable error pattern");
        }

        uint32_t elapsed_ms =
            (uint32_t)((now - pattern_started_at) * portTICK_PERIOD_MS);
        bool led_on = false;

        if (error_pattern_active)
        {
            /* Three 80 ms flashes, then return to the current normal state. */
            if (elapsed_ms < 480U)
            {
                led_on = (elapsed_ms % 160U) < 80U;
            }
            else
            {
                error_pattern_active = false;
                pattern_started_at = now;
            }
        }

        if (!error_pattern_active)
        {
            elapsed_ms =
                (uint32_t)((now - pattern_started_at) * portTICK_PERIOD_MS);
            switch (state)
            {
            case STATUS_LED_INITIALIZING:
                led_on = (elapsed_ms % 200U) < 100U;
                break;
            case STATUS_LED_DISCOVERABLE:
                led_on = (elapsed_ms % 1000U) < 500U;
                break;
            case STATUS_LED_CONNECTED:
                led_on = true;
                break;
            case STATUS_LED_STREAMING:
                led_on = (elapsed_ms % 1000U) < 920U;
                break;
            case STATUS_LED_ERROR:
                led_on = (elapsed_ms % 160U) < 80U;
                break;
            default:
                led_on = false;
                break;
            }
        }

        status_led_write(led_on);
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_TASK_PERIOD_MS));
    }
}

esp_err_t status_led_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK)
    {
        return err;
    }

    status_led_write(false);
    ESP_LOGI(TAG,
             "Status LED configured: GPIO%d, active_%s",
             STATUS_LED_GPIO,
             STATUS_LED_ACTIVE_LEVEL ? "high" : "low");

    BaseType_t result = xTaskCreate(
        status_led_task,
        "status_led",
        3072,
        NULL,
        2,
        NULL);
    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Cannot create status LED task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Task created: status_led");
    return ESP_OK;
}
