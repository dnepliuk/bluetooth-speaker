#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* All calls belong to the writer task, pinned to core 1 (including init/IRQ). */
esp_err_t control_i2s_init(void);
esp_err_t control_i2s_set_rate(uint32_t sample_rate);
esp_err_t control_i2s_write(const uint8_t *data, size_t size, size_t *written);
