#include "kb_hw.h"
#include "kb_regs.h"
#include "ch32v00x.h"

/*
 * Matrix (HW V2 / 2026-10-19):
 *   rows out PP: PC3 PC4 PC5 PD6
 *   cols in:     PC6 PC7 PD2 PD5 (+ board 10k→GND on col1–4)
 *   invert=1: row idle L / drive H, col active H
 *
 * Encoder: PD4/PD3 soft-quad (A/B have 10k↑ only; no 100n on V2)
 * INT#: PC0 push-pull, idle high, assert low (board R39 10k↑)
 * XSMT: PD0 → PCM5102 (board R27 10k↓ default mute)
 */

#define DEBOUNCE_MS  2
/* External 10k col pulldowns settle faster than IPD-only V1; tune if ghosting. */
#define MATRIX_SETTLE_NOPS  400u

static const uint16_t ROW_PC_MASK = GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5;
static const uint16_t ROW_PD_MASK = GPIO_Pin_6;
static const uint16_t COL_PC_MASK = GPIO_Pin_6 | GPIO_Pin_7;
static const uint16_t COL_PD_MASK = GPIO_Pin_2 | GPIO_Pin_5;

static uint16_t s_raw;
static uint16_t s_stable;
static uint16_t s_reported;
static uint8_t s_debounce;
static uint8_t s_mask_changed;
static uint8_t s_invert; /* 0: row idle H drive L, col active L; 1: opposite */

static int16_t s_enc_delta;  /* pending clear-on-read for host (detents) */
static int16_t s_enc_pos;    /* absolute detents */
static int8_t s_enc_edge;    /* accumulate soft-quad edges; ±4 = 1 detent */
static uint8_t s_enc_ab;     /* last A/B sample */
static uint8_t s_xsmt_unmute;

static void rows_all_idle(void)
{
    if (s_invert) {
        GPIOC->BCR = ROW_PC_MASK;
        GPIOD->BCR = ROW_PD_MASK;
    } else {
        GPIOC->BSHR = ROW_PC_MASK;
        GPIOD->BSHR = ROW_PD_MASK;
    }
}

static void row_drive_active(uint8_t row)
{
    rows_all_idle();
    if (s_invert) {
        switch (row) {
        case 0: GPIOC->BSHR = GPIO_Pin_3; break;
        case 1: GPIOC->BSHR = GPIO_Pin_4; break;
        case 2: GPIOC->BSHR = GPIO_Pin_5; break;
        case 3: GPIOD->BSHR = GPIO_Pin_6; break;
        default: break;
        }
    } else {
        switch (row) {
        case 0: GPIOC->BCR = GPIO_Pin_3; break;
        case 1: GPIOC->BCR = GPIO_Pin_4; break;
        case 2: GPIOC->BCR = GPIO_Pin_5; break;
        case 3: GPIOD->BCR = GPIO_Pin_6; break;
        default: break;
        }
    }
}

static uint8_t read_cols(void)
{
    uint8_t v = 0;
    if (s_invert) {
        /* active high on cols */
        if (GPIOC->INDR & GPIO_Pin_6) v |= (1u << 0);
        if (GPIOC->INDR & GPIO_Pin_7) v |= (1u << 1);
        if (GPIOD->INDR & GPIO_Pin_2) v |= (1u << 2);
        if (GPIOD->INDR & GPIO_Pin_5) v |= (1u << 3);
    } else {
        if ((GPIOC->INDR & GPIO_Pin_6) == 0) v |= (1u << 0);
        if ((GPIOC->INDR & GPIO_Pin_7) == 0) v |= (1u << 1);
        if ((GPIOD->INDR & GPIO_Pin_2) == 0) v |= (1u << 2);
        if ((GPIOD->INDR & GPIO_Pin_5) == 0) v |= (1u << 3);
    }
    return v;
}

void Matrix_Init(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD, ENABLE);

    g.GPIO_Speed = GPIO_Speed_30MHz;
    g.GPIO_Mode = GPIO_Mode_Out_PP;
    g.GPIO_Pin = ROW_PC_MASK;
    GPIO_Init(GPIOC, &g);
    g.GPIO_Pin = ROW_PD_MASK;
    GPIO_Init(GPIOD, &g);

    s_invert = 0;
    rows_all_idle();

    g.GPIO_Mode = GPIO_Mode_IPU;
    g.GPIO_Pin = COL_PC_MASK;
    GPIO_Init(GPIOC, &g);
    g.GPIO_Pin = COL_PD_MASK;
    GPIO_Init(GPIOD, &g);

    s_raw = s_stable = s_reported = 0;
    s_debounce = 0;
    s_mask_changed = 0;

    /* Board diode polarity needs inverted scan (confirmed on bring-up). */
    Matrix_SetInvert(1);
}

void Matrix_SetInvert(uint8_t inv)
{
    GPIO_InitTypeDef g = {0};

    s_invert = inv ? 1u : 0u;
    rows_all_idle();

    g.GPIO_Speed = GPIO_Speed_30MHz;
    g.GPIO_Mode = s_invert ? GPIO_Mode_IPD : GPIO_Mode_IPU;
    g.GPIO_Pin = COL_PC_MASK;
    GPIO_Init(GPIOC, &g);
    g.GPIO_Pin = COL_PD_MASK;
    GPIO_Init(GPIOD, &g);

    s_raw = s_stable = s_reported = 0;
    s_debounce = 0;
    s_mask_changed = 0;
}

uint8_t Matrix_GetInvert(void)
{
    return s_invert;
}

void Matrix_ScanTick(void)
{
    uint16_t mask = 0;
    uint8_t row;

    for (row = 0; row < 4; row++) {
        row_drive_active(row);
        {
            volatile uint32_t n = MATRIX_SETTLE_NOPS;
            while (n--) {
                __NOP();
            }
        }
        mask |= ((uint16_t)read_cols()) << (row * 4);
    }
    rows_all_idle();

    if (mask != s_raw) {
        s_raw = mask;
        s_debounce = 0;
        return;
    }

    if (s_debounce < DEBOUNCE_MS) {
        s_debounce++;
        return;
    }

    if (mask != s_stable) {
        s_stable = mask;
        if (s_stable != s_reported) {
            s_mask_changed = 1;
        }
    }
}

uint16_t Matrix_GetMask(void)
{
    return s_stable;
}

uint8_t Matrix_TakeChanged(void)
{
    uint8_t c = s_mask_changed;
    if (c) {
        s_mask_changed = 0;
        s_reported = s_stable;
    }
    return c;
}

void Encoder_Init(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD, ENABLE);

    /* Soft-quad: V2 A/B = 10k↑ only (no 100n). IPU as backup. */
    g.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4;
    g.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOD, &g);

    s_enc_ab = 0;
    if (GPIOD->INDR & GPIO_Pin_4) {
        s_enc_ab |= 1u;
    }
    if (GPIOD->INDR & GPIO_Pin_3) {
        s_enc_ab |= 2u;
    }
    s_enc_delta = 0;
    s_enc_pos = 0;
    s_enc_edge = 0;
}

void Encoder_Poll(void)
{
    /* gray code: old<<2|new → step -1/0/+1; EC11 detent ≈ 4 edges */
    static const int8_t tbl[16] = {
        0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0
    };
    uint8_t ab = 0;
    if (GPIOD->INDR & GPIO_Pin_4) {
        ab |= 1u; /* A */
    }
    if (GPIOD->INDR & GPIO_Pin_3) {
        ab |= 2u; /* B */
    }
    if (ab == s_enc_ab) {
        return;
    }
    {
        int8_t step = tbl[(s_enc_ab << 2) | ab];
        s_enc_ab = ab;
        if (step == 0) {
            return;
        }
        s_enc_edge = (int8_t)(s_enc_edge + step);
        while (s_enc_edge >= 4) {
            s_enc_edge = (int8_t)(s_enc_edge - 4);
            s_enc_pos = (int16_t)(s_enc_pos + 1);
            if (s_enc_delta < 127) {
                s_enc_delta = (int16_t)(s_enc_delta + 1);
            }
        }
        while (s_enc_edge <= -4) {
            s_enc_edge = (int8_t)(s_enc_edge + 4);
            s_enc_pos = (int16_t)(s_enc_pos - 1);
            if (s_enc_delta > -128) {
                s_enc_delta = (int16_t)(s_enc_delta - 1);
            }
        }
    }
}

int8_t Encoder_PeekDelta(void)
{
    return (int8_t)s_enc_delta;
}

int8_t Encoder_TakeDelta(void)
{
    int8_t d = (int8_t)s_enc_delta;
    s_enc_delta = 0;
    return d;
}

int16_t Encoder_GetPos(void)
{
    return s_enc_pos;
}

uint8_t Encoder_GetAb(void)
{
    return s_enc_ab;
}

void IntPin_Init(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    g.GPIO_Pin = GPIO_Pin_0;
    g.GPIO_Mode = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_30MHz;
    GPIO_Init(GPIOC, &g);
    GPIOC->BSHR = GPIO_Pin_0; /* idle high */
}

void IntPin_Set(uint8_t asserted)
{
    if (asserted) {
        GPIOC->BCR = GPIO_Pin_0;
    } else {
        GPIOC->BSHR = GPIO_Pin_0;
    }
}

void Charge_Init(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);
    /* Ensure PA1/PA2 are GPIO (not OSCIN/OSCOUT). Safe with HSI clock. */
    AFIO->PCFR1 &= ~(1u << 15);

    /* PA1=STDBY, PA2=CHARG — board dividers drive these; no pull */
    g.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_2;
    g.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &g);
}

uint8_t Charge_ReadRaw(void)
{
    uint8_t v = 0;
    if (GPIOA->INDR & GPIO_Pin_2) {
        v |= KB_CHG_RAW_CHARG;   /* PA2 CHARG high */
    }
    if (GPIOA->INDR & GPIO_Pin_1) {
        v |= KB_CHG_RAW_STDBY;   /* PA1 STDBY high */
    }
    return v;
}

uint8_t Charge_ReadFlags(void)
{
    uint8_t raw = Charge_ReadRaw();
    uint8_t charg_hi = (raw & KB_CHG_RAW_CHARG) != 0;
    uint8_t stdby_hi = (raw & KB_CHG_RAW_STDBY) != 0;
    uint8_t flags = 0;

    /* TP4056 OD status via dividers from VBUS:
     * charging: CHRG# low, STDBY# float→high
     * full:     STDBY# low, CHRG# float→high
     * no VBUS:  both low
     */
    if (!charg_hi && stdby_hi) {
        flags |= KB_CHG_FLAG_CHARGING;
    }
    if (!stdby_hi && charg_hi) {
        flags |= KB_CHG_FLAG_FULL;
    }
    if (charg_hi || stdby_hi) {
        flags |= KB_CHG_FLAG_VBUS;
    }
    return flags;
}

void Xsmt_Init(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD, ENABLE);
    /* Start muted (PD0 low). Board R27 also pulls down if float. */
    g.GPIO_Pin = GPIO_Pin_0;
    g.GPIO_Mode = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_30MHz;
    GPIO_Init(GPIOD, &g);
    GPIOD->BCR = GPIO_Pin_0;
    s_xsmt_unmute = 0;
}

void Xsmt_Set(uint8_t unmute)
{
    s_xsmt_unmute = unmute ? 1u : 0u;
    if (s_xsmt_unmute) {
        GPIOD->BSHR = GPIO_Pin_0;
    } else {
        GPIOD->BCR = GPIO_Pin_0;
    }
}

uint8_t Xsmt_Get(void)
{
    return s_xsmt_unmute;
}
