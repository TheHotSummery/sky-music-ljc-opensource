idf_component_register(
    SRCS "rgb_led.c" "led_strip_encoder.c"
    INCLUDE_DIRS "." "include"
    REQUIRES driver board audio_mixer key_link
)
