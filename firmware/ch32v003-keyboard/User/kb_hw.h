#pragma once

#include <stdint.h>

void Matrix_Init(void);
void Matrix_ScanTick(void);          /* call ~1 kHz / every 1 ms */
uint16_t Matrix_GetMask(void);       /* bit0=K1 .. bit14=K15, bit15=ENC SW */
uint8_t Matrix_TakeChanged(void);    /* 1 if mask changed since last take */

void Encoder_Init(void);
void Encoder_Poll(void);             /* accumulate into abs + delta */
int8_t Encoder_PeekDelta(void);
int8_t Encoder_TakeDelta(void);      /* clear-on-read clamped int8 */
int16_t Encoder_GetPos(void);        /* absolute position */
uint8_t Encoder_GetAb(void);         /* bit0=A(PD4) bit1=B(PD3) raw */

void IntPin_Init(void);
void IntPin_Set(uint8_t asserted);   /* 1=low, 0=high */

void Charge_Init(void);
uint8_t Charge_ReadRaw(void);        /* KB_CHG_RAW_* bits, 1=pin high */
uint8_t Charge_ReadFlags(void);      /* KB_CHG_FLAG_* */

void Matrix_SetInvert(uint8_t inv);  /* 0=normal, 1=inverted polarity */
uint8_t Matrix_GetInvert(void);

/* PCM5102 XSMT on PD0 (board 10k pulldown → default mute). unmute=1 → PD0 high. */
void Xsmt_Init(void);
void Xsmt_Set(uint8_t unmute);
uint8_t Xsmt_Get(void);
