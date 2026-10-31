#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rgb_led_start(void);
/* Bit0 = key1. Nonzero masks replace the note envelope: hold stays lit, flash blinks. */
void rgb_led_hint(uint16_t hold_mask, uint16_t flash_mask);

#ifdef __cplusplus
}
#endif
