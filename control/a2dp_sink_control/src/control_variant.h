#pragma once

/* One integer selector cannot combine variants. CMake and C must also agree. */
#if !defined(CONTROL_VARIANT) || CONTROL_VARIANT < 1 || CONTROL_VARIANT > 9
#error "Select exactly one CONTROL_VARIANT: 1 baseline, 2 main-config, 3 streambuffer, 4 i2s-silence, 5 pipeline, 6 clocked-pipeline, 7 spsc-pipeline, 8 spsc-dma-aligned, 9 official-i2s-reference"
#endif
#if !defined(CONTROL_CMAKE_VARIANT) || CONTROL_VARIANT != CONTROL_CMAKE_VARIANT
#error "PlatformIO build_flags and board_build.cmake_extra_args disagree"
#endif

/* Reject lists/expressions too: GCC's #if accepts a comma expression such as
 * 8,9. Require one literal selector token in both build systems. */
#define CONTROL_VALID_1_TOKEN 1
#define CONTROL_VALID_2_TOKEN 1
#define CONTROL_VALID_3_TOKEN 1
#define CONTROL_VALID_4_TOKEN 1
#define CONTROL_VALID_5_TOKEN 1
#define CONTROL_VALID_6_TOKEN 1
#define CONTROL_VALID_7_TOKEN 1
#define CONTROL_VALID_8_TOKEN 1
#define CONTROL_VALID_9_TOKEN 1
#define CONTROL_SELECTOR_TOKEN_IMPL(value) CONTROL_VALID_##value##_TOKEN
#define CONTROL_SELECTOR_TOKEN(value) CONTROL_SELECTOR_TOKEN_IMPL(value)
#if CONTROL_SELECTOR_TOKEN(CONTROL_VARIANT) != 1 || CONTROL_SELECTOR_TOKEN(CONTROL_CMAKE_VARIANT) != 1
#error "Use one literal CONTROL_VARIANT integer, without lists or expressions"
#endif

#define CONTROL_DMA_ALIGNED (CONTROL_VARIANT == 8)
#define CONTROL_SPSC (CONTROL_VARIANT == 7 || CONTROL_DMA_ALIGNED)
#define CONTROL_CLOCKED (CONTROL_VARIANT == 6 || CONTROL_SPSC)
#if CONTROL_CLOCKED
/* Historical flag means buffered PCM callback; variants 7/8 use the SPSC backend.
 * Keeping this interface preserves main.c and its Bluetooth/PCM callbacks. */
#define CONTROL_HAS_STREAM 1
#define CONTROL_HAS_I2S 1
#else
/* Preserve the original expansions for existing builds as well as behavior. */
#define CONTROL_HAS_STREAM (CONTROL_VARIANT == 3 || CONTROL_VARIANT == 5)
#define CONTROL_HAS_I2S (CONTROL_VARIANT == 4 || CONTROL_VARIANT == 5)
#endif
#define CONTROL_HAS_TRANSPORT (CONTROL_HAS_STREAM || CONTROL_HAS_I2S)

#if CONTROL_VARIANT == 1
#define CONTROL_VARIANT_NAME "baseline"
#elif CONTROL_VARIANT == 2
#define CONTROL_VARIANT_NAME "main-config"
#elif CONTROL_VARIANT == 3
#define CONTROL_VARIANT_NAME "streambuffer"
#elif CONTROL_VARIANT == 4
#define CONTROL_VARIANT_NAME "i2s-silence"
#elif CONTROL_VARIANT == 5
#define CONTROL_VARIANT_NAME "pipeline"
#elif CONTROL_VARIANT == 6
#define CONTROL_VARIANT_NAME "clocked-pipeline"
#elif CONTROL_VARIANT == 7
#define CONTROL_VARIANT_NAME "spsc-pipeline"
#elif CONTROL_VARIANT == 8
#define CONTROL_VARIANT_NAME "spsc-dma-aligned"
#elif CONTROL_VARIANT == 9
#define CONTROL_VARIANT_NAME "official-i2s-reference"
#endif
