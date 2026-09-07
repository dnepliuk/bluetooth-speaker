#pragma once

#include "esp_err.h"

typedef enum
{
    STATUS_LED_INITIALIZING,
    STATUS_LED_DISCOVERABLE,
    STATUS_LED_CONNECTED,
    STATUS_LED_STREAMING,
    STATUS_LED_ERROR
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set_state(status_led_state_t state);
void status_led_signal_error(void);
