idf_component_register(
    SRCS "mixer.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board audio_i2s esp_mm
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
