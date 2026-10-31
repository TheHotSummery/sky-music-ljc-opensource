idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board audio_i2s audio_mixer key_link audio_bank rgb_led encoder enc_fn ui_tft score_play sd_music net_link drill ble_link touch_cst816 mic_i2s sky_proto
)
