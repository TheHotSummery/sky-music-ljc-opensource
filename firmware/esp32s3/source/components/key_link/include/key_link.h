#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t key_link_start(void);
int key_link_ok(void);
esp_err_t key_link_whoami(uint8_t *who, uint8_t *ver);
uint16_t key_link_keymask(void);
void key_link_dump_power(void); /* bat_ok/mv/soc + chg flags from CH32 */
int key_link_bat_ok(void);
uint32_t key_link_bat_mv(void);
uint8_t key_link_bat_soc(void); /* UI SOC (voltage-mapped + filtered) */
uint8_t key_link_bat_soc_chip(void); /* raw CW2015 SOC */
uint8_t key_link_chg_flags(void);
int key_link_bat_alrt(void); /* GPIO level: 1=ok (pull-up), 0=alert */
/* 0=ok 1=low(≤20%/~3.5V) 2=critical(≤3.05V or ALRT) */
int key_link_bat_level(void);
const char *key_link_chg_name(void);
/* PCM5102 soft mute via CH32 CTRL bit1. 1=muted. */
int key_link_xsmt_muted(void);
/* unmute=1 clears mute (normal play); unmute=0 forces mute. */
esp_err_t key_link_xsmt_set(int unmute);

/* Shared I2C1 bus (CH32 0x28 + CW2015 0x62 + CST816D 0x15). */
i2c_master_bus_handle_t key_link_i2c_bus(void);
void key_link_i2c_lock(void);
void key_link_i2c_unlock(void);

#ifdef __cplusplus
}
#endif
