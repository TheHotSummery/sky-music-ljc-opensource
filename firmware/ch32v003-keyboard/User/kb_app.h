#pragma once

#include <stdint.h>
#include "kb_regs.h"

void Kb_Init(void);
void Kb_Task(void);                  /* scan + encoder + refresh status/INT */

uint8_t Kb_ReadReg(uint8_t reg);
void Kb_WriteReg(uint8_t reg, uint8_t val);
void Kb_OnMasterReadDone(void);      /* optional: after STOP on read */

void I2C_Slave_Init(void);
