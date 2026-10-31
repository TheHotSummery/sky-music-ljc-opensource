idf_component_register(
    SRCS "ble_link.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash ui_tft board sky_proto sky_session net_link enc_fn
)
