#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_pipeline_init(void);
void audio_pipeline_receive_pcm(const uint8_t *data, uint32_t length);
void audio_pipeline_set_streaming(bool streaming);
void audio_pipeline_set_sample_rate(uint32_t sample_rate_hz);
void audio_pipeline_update_rssi(int8_t rssi_delta, bool valid);
