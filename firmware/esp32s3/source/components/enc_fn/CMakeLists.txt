idf_component_register(
    SRCS "enc_fn.c"
    INCLUDE_DIRS "include"
    REQUIRES board audio_bank sd_music score_play audio_mixer drill esp_common
)
