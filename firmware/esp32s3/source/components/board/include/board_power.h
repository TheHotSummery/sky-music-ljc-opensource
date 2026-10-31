#pragma once

#include "esp_err.h"

esp_err_t board_power_init(void);
void board_boost_set(int on);
int board_boost_get(void);
