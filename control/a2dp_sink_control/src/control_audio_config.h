#pragma once

/* Minimal parameter snapshot from src/app_config.h at 8a36a8c520d7.
 * These are local constants: no build dependency on the main application.
 */
#define CONTROL_SAMPLE_RATE 44100U
#define CONTROL_FRAME_BYTES 4U
#define CONTROL_STREAM_BYTES (32U * 1024U)
#if CONTROL_VARIANT == 8
/* The sole functional A/B change: one output block equals the existing DMA
 * descriptor. Keep prefill, DMA geometry and every older variant unchanged. */
#define CONTROL_CHUNK_BYTES 3840U
#else
#define CONTROL_CHUNK_BYTES 4096U
#endif
#define CONTROL_PREFILL_BYTES 8192U
#define CONTROL_WRITER_PRIORITY (configMAX_PRIORITIES - 3)
#define CONTROL_WRITER_CORE 1
#define CONTROL_WRITER_STACK 8192U
#define CONTROL_RECEIVE_TIMEOUT_MS 100U
#define CONTROL_WRITE_TIMEOUT_MS 1000U
#define CONTROL_DMA_DESCRIPTORS 3U
#define CONTROL_DMA_FRAMES 960U

#if CONTROL_STREAM_BYTES % CONTROL_FRAME_BYTES != 0 || \
    CONTROL_CHUNK_BYTES % CONTROL_FRAME_BYTES != 0 || \
    CONTROL_PREFILL_BYTES % CONTROL_FRAME_BYTES != 0 || \
    CONTROL_PREFILL_BYTES >= CONTROL_STREAM_BYTES
#error "Audio buffers must preserve stereo frames and leave room beyond prefill"
#endif

#if CONTROL_DMA_FRAMES * CONTROL_FRAME_BYTES > 4092U
#error "I2S DMA descriptor exceeds the ESP32 aligned size limit"
#endif
