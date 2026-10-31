idf_component_register(
    SRCS "mic_i2s.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board audio_mixer key_link
)
