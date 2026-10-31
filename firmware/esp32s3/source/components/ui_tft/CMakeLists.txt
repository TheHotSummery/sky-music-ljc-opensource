idf_component_register(
    SRCS "ui_tft.c" "ui_theme.c" "ui_bg.c" "font_cn.c" "font_cn_data.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd board audio_mixer usb_ident audio_bank enc_fn key_link sd_music score_play drill mic_i2s
)
