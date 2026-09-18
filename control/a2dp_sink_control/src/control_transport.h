#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t control_transport_init(void);
void control_transport_set_streaming(bool streaming);
void control_transport_set_format(uint32_t rate, bool stereo);
void control_transport_send(const uint8_t *data, uint32_t len);
/* Last operation in the buffered callback; no contended lock after its timer. */
void control_transport_callback_done(int64_t entry_us);
/* Called only with the existing five-second PCM stats report. */
void control_transport_log_stats(void);
