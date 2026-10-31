#pragma once

#include <stdint.h>

/*
 * CH32V003 keyboard I2C slave (7-bit addr 0x28)
 * Keep in sync with ch32v003-keyboard/User/kb_regs.h
 */

#define KB_I2C_ADDR_7BIT        0x28u

#define KB_REG_WHOAMI           0x00u
#define KB_REG_VERSION          0x01u
#define KB_REG_STATUS           0x02u
#define KB_REG_KEYMASK_L        0x03u
#define KB_REG_KEYMASK_H        0x04u
#define KB_REG_ENC_DELTA        0x05u
#define KB_REG_EVT_COUNT        0x06u
#define KB_REG_EVT_DATA         0x07u
#define KB_REG_CHG_RAW          0x08u
#define KB_REG_CHG_FLAGS        0x09u
#define KB_REG_CTRL             0x0Au
#define KB_REG_ENC_POS_L        0x0Bu
#define KB_REG_ENC_POS_H        0x0Cu

#define KB_WHOAMI_VALUE         0xCBu
#define KB_VERSION_VALUE        0x07u

#define KB_STATUS_KEY           0x01u
#define KB_STATUS_ENC           0x02u
#define KB_STATUS_EVT           0x04u
#define KB_CTRL_INV_MATRIX      0x01u
#define KB_CTRL_XSMT_MUTE       0x02u  /* 1=mute DAC; 0=unmute */

#define KB_CHG_RAW_CHARG        0x01u
#define KB_CHG_RAW_STDBY        0x02u
#define KB_CHG_FLAG_CHARGING    0x01u
#define KB_CHG_FLAG_FULL        0x02u
#define KB_CHG_FLAG_VBUS        0x04u

#define KB_KEY_COUNT            16u
#define KB_KEY_ENC_SW           15u
