#include "kb_app.h"
#include "kb_hw.h"
#include "ch32v00x.h"

typedef struct {
    uint8_t key;
    uint8_t state;
} KbEvt;

static volatile uint8_t s_reg_ptr;
static volatile uint8_t s_status;
static volatile uint8_t s_got_addr;
static volatile uint8_t s_evt_phase;

static KbEvt s_fifo[KB_EVT_FIFO_DEPTH];
static volatile uint8_t s_fifo_head;
static volatile uint8_t s_fifo_tail;
static volatile uint8_t s_fifo_count;

static uint16_t s_last_mask;

void I2C1_EV_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_ER_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

static void refresh_int(void)
{
    if (Encoder_PeekDelta() != 0) {
        s_status |= KB_STATUS_ENC;
    }
    if (s_fifo_count != 0) {
        s_status |= KB_STATUS_EVT;
    }
    IntPin_Set((s_status & (KB_STATUS_KEY | KB_STATUS_ENC | KB_STATUS_EVT)) != 0);
}

static void fifo_push(uint8_t key, uint8_t state)
{
    if (s_fifo_count >= KB_EVT_FIFO_DEPTH) {
        s_fifo_tail = (uint8_t)((s_fifo_tail + 1) % KB_EVT_FIFO_DEPTH);
        s_fifo_count--;
    }
    s_fifo[s_fifo_head].key = key;
    s_fifo[s_fifo_head].state = state;
    s_fifo_head = (uint8_t)((s_fifo_head + 1) % KB_EVT_FIFO_DEPTH);
    s_fifo_count++;
    s_status |= KB_STATUS_EVT;
}

static uint8_t fifo_pop_byte(void)
{
    if (s_fifo_count == 0) {
        s_evt_phase = 0;
        return 0xFF;
    }
    if (s_evt_phase == 0) {
        s_evt_phase = 1;
        return s_fifo[s_fifo_tail].key;
    }

    {
        uint8_t st = s_fifo[s_fifo_tail].state;
        s_fifo_tail = (uint8_t)((s_fifo_tail + 1) % KB_EVT_FIFO_DEPTH);
        s_fifo_count--;
        s_evt_phase = 0;
        if (s_fifo_count == 0) {
            s_status &= (uint8_t)~KB_STATUS_EVT;
        }
        return st;
    }
}

static void emit_mask_edges(uint16_t now)
{
    uint16_t changed = (uint16_t)(now ^ s_last_mask);
    uint8_t i;
    for (i = 0; i < KB_KEY_COUNT; i++) {
        if (changed & (1u << i)) {
            fifo_push(i, (now & (1u << i)) ? 1u : 0u);
        }
    }
    s_last_mask = now;
}

static uint8_t i2c_tx_byte(void)
{
    uint8_t reg = s_reg_ptr;
    uint8_t v = Kb_ReadReg(reg);
    /* EVT_DATA stays on 0x07 so host can burst-pop events */
    if (reg != KB_REG_EVT_DATA) {
        s_reg_ptr = (uint8_t)(reg + 1u);
    }
    return v;
}

uint8_t Kb_ReadReg(uint8_t reg)
{
    uint16_t mask = Matrix_GetMask();

    switch (reg) {
    case KB_REG_WHOAMI:
        return KB_WHOAMI_VALUE;
    case KB_REG_VERSION:
        return KB_VERSION_VALUE;
    case KB_REG_STATUS:
        if (Encoder_PeekDelta() != 0) {
            s_status |= KB_STATUS_ENC;
        }
        if (s_fifo_count) {
            s_status |= KB_STATUS_EVT;
        }
        return s_status;
    case KB_REG_KEYMASK_L:
        s_status &= (uint8_t)~KB_STATUS_KEY;
        return (uint8_t)(mask & 0xFFu);
    case KB_REG_KEYMASK_H:
        s_status &= (uint8_t)~KB_STATUS_KEY;
        return (uint8_t)((mask >> 8) & 0xFFu);
    case KB_REG_ENC_DELTA: {
        int8_t d = Encoder_TakeDelta();
        s_status &= (uint8_t)~KB_STATUS_ENC;
        return (uint8_t)d;
    }
    case KB_REG_EVT_COUNT:
        return s_fifo_count;
    case KB_REG_EVT_DATA:
        return fifo_pop_byte();
    case KB_REG_CHG_RAW:
        return Charge_ReadRaw();
    case KB_REG_CHG_FLAGS:
        return Charge_ReadFlags();
    case KB_REG_CTRL: {
        uint8_t v = 0;
        if (Matrix_GetInvert()) {
            v |= KB_CTRL_INV_MATRIX;
        }
        if (!Xsmt_Get()) {
            v |= KB_CTRL_XSMT_MUTE;
        }
        return v;
    }
    case KB_REG_ENC_POS_L:
        return (uint8_t)(Encoder_GetPos() & 0xFFu);
    case KB_REG_ENC_POS_H:
        return (uint8_t)((Encoder_GetPos() >> 8) & 0xFFu);
    default:
        return 0x00;
    }
}

void Kb_WriteReg(uint8_t reg, uint8_t val)
{
    if (reg == KB_REG_STATUS && val == 0) {
        s_status &= (uint8_t)~(KB_STATUS_KEY | KB_STATUS_ENC);
        refresh_int();
    }
    if (reg == KB_REG_CTRL) {
        Matrix_SetInvert((val & KB_CTRL_INV_MATRIX) != 0);
        /* bit1 set = mute; clear = unmute */
        Xsmt_Set((val & KB_CTRL_XSMT_MUTE) == 0);
    }
}

void Kb_OnMasterReadDone(void)
{
    refresh_int();
}

void Kb_Init(void)
{
    s_reg_ptr = 0;
    s_status = 0;
    s_got_addr = 1;
    s_evt_phase = 0;
    s_fifo_head = s_fifo_tail = s_fifo_count = 0;
    s_last_mask = 0;

    Matrix_Init();
    Encoder_Init();
    Charge_Init();
    IntPin_Init();
    Xsmt_Init();           /* PD0 low = muted during bring-up */
    I2C_Slave_Init();
    Xsmt_Set(1);           /* ready → unmute PCM5102 */
}

void Kb_Task(void)
{
    uint16_t mask;

    Matrix_ScanTick();
    Encoder_Poll();

    mask = Matrix_GetMask();
    if (Matrix_TakeChanged()) {
        emit_mask_edges(mask);
        s_status |= KB_STATUS_KEY;
    }
    if (Encoder_PeekDelta() != 0) {
        s_status |= KB_STATUS_ENC;
    }
    refresh_int();
}

void I2C_Slave_Init(void)
{
    GPIO_InitTypeDef g = {0};
    I2C_InitTypeDef i2c = {0};
    NVIC_InitTypeDef nvic = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    g.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_2;
    g.GPIO_Mode = GPIO_Mode_AF_OD;
    g.GPIO_Speed = GPIO_Speed_30MHz;
    GPIO_Init(GPIOC, &g);

    i2c.I2C_ClockSpeed = 100000;
    i2c.I2C_Mode = I2C_Mode_I2C;
    i2c.I2C_DutyCycle = I2C_DutyCycle_16_9;
    i2c.I2C_OwnAddress1 = KB_I2C_OWN_ADDR;
    i2c.I2C_Ack = I2C_Ack_Enable;
    i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C1, &i2c);
    I2C_Cmd(I2C1, ENABLE);
    I2C_AcknowledgeConfig(I2C1, ENABLE);

    nvic.NVIC_IRQChannel = I2C1_EV_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    nvic.NVIC_IRQChannel = I2C1_ER_IRQn;
    nvic.NVIC_IRQChannelSubPriority = 1;
    NVIC_Init(&nvic);

    I2C_ITConfig(I2C1, I2C_IT_BUF | I2C_IT_EVT | I2C_IT_ERR, ENABLE);
}

void I2C1_EV_IRQHandler(void)
{
    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_ADDR) != RESET) {
        uint32_t sr2 = I2C1->STAR2; /* clear ADDR */
        if ((sr2 & I2C_STAR2_TRA) == 0) {
            s_got_addr = 1;
        }
        return;
    }

    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_RXNE) != RESET) {
        uint8_t b = I2C_ReceiveData(I2C1);
        if (s_got_addr) {
            s_reg_ptr = b;
            s_got_addr = 0;
        } else {
            Kb_WriteReg(s_reg_ptr, b);
            s_reg_ptr++;
        }
        return;
    }

    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_TXE) != RESET) {
        I2C_SendData(I2C1, i2c_tx_byte());
        return;
    }

    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_STOPF) != RESET) {
        /* clear STOPF: read STAR1 then write CTLR1 */
        (void)I2C1->STAR1;
        I2C1->CTLR1 |= I2C_CTLR1_PE;
        s_got_addr = 1;
        Kb_OnMasterReadDone();
        return;
    }

    if (I2C_GetFlagStatus(I2C1, I2C_FLAG_BTF) != RESET) {
        (void)I2C_ReceiveData(I2C1);
    }
}

void I2C1_ER_IRQHandler(void)
{
    if (I2C_GetITStatus(I2C1, I2C_IT_AF)) {
        I2C_ClearITPendingBit(I2C1, I2C_IT_AF);
        s_got_addr = 1;
        Kb_OnMasterReadDone();
    }
    if (I2C_GetITStatus(I2C1, I2C_IT_BERR)) {
        I2C_ClearITPendingBit(I2C1, I2C_IT_BERR);
    }
    if (I2C_GetITStatus(I2C1, I2C_IT_ARLO)) {
        I2C_ClearITPendingBit(I2C1, I2C_IT_ARLO);
    }
    if (I2C_GetITStatus(I2C1, I2C_IT_OVR)) {
        I2C_ClearITPendingBit(I2C1, I2C_IT_OVR);
    }
}
