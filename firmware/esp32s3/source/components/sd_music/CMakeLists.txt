idf_component_register(
    SRCS "sd_music.c"
    INCLUDE_DIRS "include"
    REQUIRES board audio_mixer fatfs freertos esp_audio_codec
)
