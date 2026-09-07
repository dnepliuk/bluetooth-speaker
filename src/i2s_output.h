#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t i2s_output_init(void);
esp_err_t i2s_output_set_sample_rate(uint32_t sample_rate_hz);
esp_err_t i2s_output_write(
    const void *data,
    size_t size,
    size_t *bytes_written);
