cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-music-keyboard)

# 出厂音色镜像：生成 assets.bin，但不挂进默认 idf.py flash（避免每次重写 ~4MB）。
# 首次/改音色/改分区表后：idf.py assets-flash
# 或：esptool write_flash 0x310000 build/assets.bin
set(FACTORY_DIR "${CMAKE_SOURCE_DIR}/assets/factory")
if(EXISTS "${FACTORY_DIR}/piano/1.wav")
    fatfs_create_spiflash_image(assets "${FACTORY_DIR}")
endif()

