#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_link_start(void);
void ble_cmd(const char *args);
void ble_dump_status(void);
void ble_link_note(uint8_t note_id, int down); /* 0..14 */
void ble_link_set_battery(uint8_t soc);        /* 0..100, CW2015 → BAS 0x2A19 */

#ifdef __cplusplus
}
#endif
