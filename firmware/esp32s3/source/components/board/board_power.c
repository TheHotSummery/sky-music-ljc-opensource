#include "board_power.h"
#include "board_pins.h"

#include "esp_log.h"

static const char *TAG = "board";

esp_err_t board_power_init(void)
{
    /* V2: no FP6276 / BOOST_EN. WS2812 + amp + backlight run from VSYS. */
    ESP_LOGI(TAG, "V2 power: no boost (VSYS). TP-INT=GPIO%d INT#=GPIO%d WS2812=GPIO%d TFT_BLK=GPIO%d",
             (int)PIN_TP_INT, (int)PIN_INT, (int)PIN_WS2812, (int)PIN_TFT_BLK);
    return ESP_OK;
}

void board_boost_set(int on)
{
    (void)on; /* stub: keep console cmds harmless */
}

int board_boost_get(void)
{
    return 1; /* always "on" — rail is VSYS */
}
