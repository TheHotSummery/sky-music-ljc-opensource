/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Description        : CH32V003 matrix keyboard + EC11 + I2C slave for ESP32-S3
 *******************************************************************************/

#include "debug.h"
#include "kb_app.h"
#include "kb_hw.h"

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    Kb_Init();

    while (1) {
        /* Sample ENC ~5 kHz — 100nF RC + 1 ms poll was dropping gray codes */
        {
            uint8_t i;
            for (i = 0; i < 5; i++) {
                Encoder_Poll();
                Delay_Us(200);
            }
        }
        Kb_Task();
    }
}