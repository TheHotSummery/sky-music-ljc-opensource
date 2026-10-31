idf_component_register(
    SRCS "key_link.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board audio_mixer enc_fn ble_link
)
