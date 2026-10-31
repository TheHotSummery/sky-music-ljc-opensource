idf_component_register(
    SRCS "sky_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES enc_fn audio_mixer key_link drill score_play sd_music net_link sky_session
)
