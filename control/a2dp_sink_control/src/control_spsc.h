#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Exactly one non-reentrant producer and one consumer for the entire lifetime.
 * No reset API: only the consumer may discard PCM, by receiving a finite prefix.
 * Init is a no-op; static storage/counters are initialized once at boot.
 */
esp_err_t control_spsc_init(void);
size_t control_spsc_send(const uint8_t *data, uint32_t len, uint32_t *send_us);
size_t control_spsc_receive(uint8_t *data, size_t size);
size_t control_spsc_producer_fill(void); /* Producer only. */
size_t control_spsc_consumer_fill(void); /* Consumer only; not a third reader. */

/* Independent atomic lifetime counters; stats task reads, never resets them. */
uint32_t control_spsc_wrap_writes(void);
uint32_t control_spsc_wrap_reads(void);
