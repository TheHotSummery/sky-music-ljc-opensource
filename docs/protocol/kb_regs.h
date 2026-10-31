#pragma once

#include <stdint.h>

/*
 * CH32V003 keyboard I2C slave (7-bit addr 0x28) — protocol v3 / VERSION 0x07
 *
 * Write: START ADDR_W [reg] [optional data...] STOP
 * Read:  START ADDR_W [reg] RESTART ADDR_R DATA... STOP
 *        Auto-increment after each read byte, except EVT_DATA (0x07) stays put.
 *
 * INT# (PC0): idle high, assert low while STATUS has KEY|ENC|EVT.
 *
 * Pins (HW V2 / 3x3+rgb 2026-10-19):
 *   I2C SDA/SCL = PC1/PC2
 *   INT#        = PC0 (+ board R39 10k↑)
 *   rows        = PC3/PC4/PC5/PD6
 *   cols        = PC6/PC7/PD2/PD5 (+ board 10k↓ each)
 *   ENC A/B     = PD4/PD3 (GPIO soft-quad; 4 edges = 1 detent)
 *   XSMT        = PD0 → PCM5102 (+ board R27 10k↓ default mute)
 *   STDBY/CHARG = PA1/PA2 (TP4056 sense; clock MUST be HSI — HSE remaps PA1/PA2)
 */

#define KB_I2C_ADDR_7BIT        0x28u
#define KB_I2C_OWN_ADDR         ((uint16_t)(KB_I2C_ADDR_7BIT << 1))

#define KB_REG_WHOAMI           0x00u
#define KB_REG_VERSION          0x01u
#define KB_REG_STATUS           0x02u
#define KB_REG_KEYMASK_L        0x03u
#define KB_REG_KEYMASK_H        0x04u
#define KB_REG_ENC_DELTA        0x05u  /* int8, clear-on-read */
#define KB_REG_EVT_COUNT        0x06u
#define KB_REG_EVT_DATA         0x07u  /* pop: key_id, state; ptr does not ++ */
#define KB_REG_CHG_RAW          0x08u  /* bit0=CHARG(PA2), bit1=STDBY(PA1), 1=high */
#define KB_REG_CHG_FLAGS        0x09u  /* decoded charge flags */
#define KB_REG_CTRL             0x0Au  /* writable control */
#define KB_REG_ENC_POS_L        0x0Bu  /* int16 abs position low (not clear-on-read) */
#define KB_REG_ENC_POS_H        0x0Cu

#define KB_WHOAMI_VALUE         0xCBu
/* v7: PD0 XSMT + CTRL mute bit; V2 col 10k settle; ENC A/B no 100n */
#define KB_VERSION_VALUE        0x07u

#define KB_STATUS_KEY           0x01u
#define KB_STATUS_ENC           0x02u
#define KB_STATUS_EVT           0x04u

#define KB_CTRL_INV_MATRIX      0x01u  /* 1=invert row/col scan polarity */
#define KB_CTRL_XSMT_MUTE       0x02u  /* 1=mute (PD0 low); 0=unmute (PD0 high) */

/* CHG_RAW bits */
#define KB_CHG_RAW_CHARG        0x01u
#define KB_CHG_RAW_STDBY        0x02u

/* CHG_FLAGS bits */
#define KB_CHG_FLAG_CHARGING    0x01u  /* TP4056 charging */
#define KB_CHG_FLAG_FULL        0x02u  /* charge complete */
#define KB_CHG_FLAG_VBUS        0x04u  /* sense rail looks powered */

/* key_id 0..14 = K1..K15; 15 = encoder switch */
#define KB_KEY_COUNT            16u
#define KB_KEY_ENC_SW           15u

#define KB_EVT_FIFO_DEPTH       32u
