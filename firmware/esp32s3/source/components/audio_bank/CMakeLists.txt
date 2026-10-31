idf_component_register(
    SRCS "audio_bank.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs wear_levelling sdmmc driver board audio_mixer freertos esp_mm
)
