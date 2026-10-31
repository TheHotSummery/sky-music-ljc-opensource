idf_component_register(
    SRCS "usb_ident.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board
    PRIV_REQUIRES esp_tinyusb
)
