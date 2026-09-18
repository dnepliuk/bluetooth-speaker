/* Adapted from the send/receive primitives in main src/audio_pipeline.c.
 * No prefetch, reset, second consumer, trim, log hook or audio processing.
 */
#include "control_stream.h"
#include "control_audio_config.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"

static StreamBufferHandle_t s_stream;

esp_err_t control_stream_init(void)
{
    s_stream = xStreamBufferCreate(CONTROL_STREAM_BYTES, CONTROL_FRAME_BYTES);
    return s_stream != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t control_stream_send(const uint8_t *data, uint32_t len, uint32_t *send_us)
{
    const int64_t start_us = esp_timer_get_time();
    size_t sent = 0;
    if (data != NULL && len != 0 && len % CONTROL_FRAME_BYTES == 0) {
        /* As in main: the sole consumer can only increase available space.
         * Round capacity down before send; drop the unsent tail, never retry.
         */
        size_t to_send = xStreamBufferSpacesAvailable(s_stream);
        to_send -= to_send % CONTROL_FRAME_BYTES;
        if (to_send > len) {
            to_send = len;
        }
        if (to_send != 0) {
            sent = xStreamBufferSend(s_stream, data, to_send, 0);
        }
    }
    *send_us = (uint32_t)(esp_timer_get_time() - start_us);
    return sent;
}

size_t control_stream_receive(uint8_t *data, size_t size, TickType_t timeout)
{
    return xStreamBufferReceive(s_stream, data, size, timeout);
}

size_t control_stream_fill(void)
{
    return xStreamBufferBytesAvailable(s_stream);
}
