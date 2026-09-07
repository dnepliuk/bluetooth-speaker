idf_component_register(
    SRCS "main.c" "bluetooth_audio.c" "audio_pipeline.c" "i2s_output.c" "status_led.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash
)
