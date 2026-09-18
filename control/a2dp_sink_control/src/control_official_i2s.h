/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */
#pragma once

#include <stdint.h>
#include "esp_a2dp_api.h"
#include "esp_err.h"

#define OFFICIAL_RING_BYTES (32 * 1024)
#define OFFICIAL_PREFETCH_BYTES (20 * 1024)
#define OFFICIAL_WRITE_UPTO (240 * 6)
#define OFFICIAL_READ_TIMEOUT_MS 20
#define OFFICIAL_WRITER_STACK (4 * 1024)
#define OFFICIAL_WRITER_PRIORITY (configMAX_PRIORITIES - 3)
#define OFFICIAL_STATS_PERIOD_MS 5000
#define OFFICIAL_BCK 26
#define OFFICIAL_WS 25
#define OFFICIAL_DATA 22

/* Lifecycle is serialized by the existing Bluetooth event callback. */
esp_err_t control_official_i2s_init(void);
esp_err_t control_official_i2s_open(void);
esp_err_t control_official_i2s_start(void);
void control_official_i2s_stop(void);
void control_official_i2s_close(void);
esp_err_t control_official_i2s_codec_update(const esp_a2d_mcc_t *mcc);

/* Bluedroid owns data. Entire packet accepted or dropped, timeout zero. */
void control_official_i2s_output(const uint8_t *data, uint32_t len);
