idf_component_register(
    SRCS "score_play.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board audio_mixer
    PRIV_REQUIRES esp_timer
)
