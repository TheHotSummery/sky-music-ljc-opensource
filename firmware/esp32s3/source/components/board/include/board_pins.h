#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"

/*
 * HW V2 board sky-music-v2 (ESP32-S3-N16R8 + CH32 I2C).
 * Schematic: SCH_3x3+rgb_2026-10-19 — pinout verified against U1 nets
 * (closer to V1 GPIO map than the 2026-10-09 draft remap).
 * No boost; WS2812/amp/backlight on VSYS.
 * GPIO33-37 OPI PSRAM. GPIO19/20 USB. GPIO0/3/45/46 strapping.
 */

#define AUDIO_SAMPLE_RATE          22050
#define AUDIO_BITS_PER_SAMPLE      16
#define AUDIO_CHANNELS_OUT         2
#define AUDIO_BLOCK_FRAMES         256
#define AUDIO_MAX_VOICES           32
#define AUDIO_DEFAULT_VOICES       16
#define AUDIO_BANK_WAV_COUNT       15

#define PIN_I2S_BCLK               GPIO_NUM_4
#define PIN_I2S_WS                 GPIO_NUM_5
#define PIN_I2S_DOUT               GPIO_NUM_6
/* PCM5102: SCK=GND; XSMT = CH32 PD0. Do not use GPIO38/39 (ENC2). */
#define PIN_I2S_MCLK               I2S_GPIO_UNUSED
#define PIN_MAX98357_SD            (-1)
#define PIN_PCM5102_XSMT           (-1)

#define PIN_SD_SCK                 GPIO_NUM_12
#define PIN_SD_MOSI                GPIO_NUM_11
#define PIN_SD_MISO                GPIO_NUM_13
#define PIN_SD_CS                  GPIO_NUM_10
#define SD_SPI_HOST                SPI2_HOST
#define SD_MOUNT_POINT             "/sdcard"
#define FACTORY_MOUNT_POINT        "/factory"
#define FACTORY_PART_LABEL         "assets"

#define PIN_I2C_SDA                GPIO_NUM_40
#define PIN_I2C_SCL                GPIO_NUM_41
#define PIN_INT                    GPIO_NUM_47
#define KEY_I2C_PORT               0
#define KEY_I2C_HZ                 100000

/* No boost on V2. GPIO2 = touch panel INT. Module TP-RST not routed to MCU. */
#define PIN_TP_INT                 GPIO_NUM_2
#define PIN_TP_RST                 (-1)
#define PIN_BAT_ALRT               GPIO_NUM_42
#define TOUCH_I2C_ADDR7            0x15u /* CST816D */
#define I2S_MIC_PORT               I2S_NUM_1

#define I2S_PORT                   I2S_NUM_0

#define TFT_SPI_HOST               SPI3_HOST
#define PIN_TFT_SCK                GPIO_NUM_8
#define PIN_TFT_MOSI               GPIO_NUM_9
#define PIN_TFT_CS                 GPIO_NUM_14
#define PIN_TFT_DC                 GPIO_NUM_15
#define PIN_TFT_RST                GPIO_NUM_16
#define PIN_TFT_BLK                GPIO_NUM_21 /* AO3400 low-side backlight */
/* ST7789P3 240x284 (GRAM 240x320). Product view: landscape 284x240 (rot=3).
 * Gate gap 36: visible GRAM rows 36..319 (else landscape shows stale strip on one side).
 * Rounded corners ~12–16px; UI should keep TFT_SAFE inset. */
#define TFT_PHYS_W                 240
#define TFT_PHYS_H                 284
#define TFT_GRAM_H                 320
#define TFT_COL_OFFSET             0
#define TFT_ROW_OFFSET             (TFT_GRAM_H - TFT_PHYS_H) /* 36 */
#define TFT_WIDTH                  TFT_PHYS_H /* landscape logical */
#define TFT_HEIGHT                 TFT_PHYS_W
#define TFT_SAFE                   14 /* keep content inside round bezel */

#define PIN_WS2812                 GPIO_NUM_48
#define WS2812_COUNT               15
#define PIN_BOOT_BTN               GPIO_NUM_0

/* ENC2 on S3; ENC1 on CH32. GPIO2 is TP-INT (not BOOST). */
#define PIN_EC11_A                 GPIO_NUM_38
#define PIN_EC11_B                 GPIO_NUM_39
#define PIN_EC11_SW                GPIO_NUM_1

/* ICS-43434 (nets still INMP_*); optional mic — not used by main app yet */
#define PIN_INMP_SCK               GPIO_NUM_7
#define PIN_INMP_WS                GPIO_NUM_17
#define PIN_INMP_SD                GPIO_NUM_18

#define BOARD_WIFI_SSID            "TOP-C Lab_2.4G"
#define BOARD_WIFI_PASS            "topc123456"
#define BOARD_TCP_PORT             2333
