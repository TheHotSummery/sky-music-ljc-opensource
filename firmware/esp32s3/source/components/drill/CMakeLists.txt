idf_component_register(
    SRCS "drill.c"
    INCLUDE_DIRS "include"
    REQUIRES score_play rgb_led board
    PRIV_REQUIRES esp_timer
)
