idf_component_register(
    SRCS "net_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_http_server mdns lwip board ui_tft audio_mixer score_play audio_bank usb_ident sky_session sd_music mbedtls
    EMBED_FILES "portal.html"
)

