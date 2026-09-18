#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t control_stream_init(void);
/* One producer, one consumer; send always uses zero timeout. */
size_t control_stream_send(const uint8_t *data, uint32_t len, uint32_t *send_us);
size_t control_stream_receive(uint8_t *data, size_t size, TickType_t timeout);
size_t control_stream_fill(void);
