#include "encoder.h"
#include "board_pins.h"
#include "mixer.h"
#include "ui_tft.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "enc2";

/* Left-hand ENC2: rotate=volume, click=mute, hold+rotate=transpose + K1 preview. */
static int s_last_a;
static int s_sw_down;
static TickType_t s_sw_at;
static uint8_t s_vol_before_mute = MIXER_MASTER_UI_DEFAULT;
static int s_held_turned;

static void set_master(uint8_t vol)
{
    MixCmd cmd = { .type = MIX_CMD_SET_MASTER, .velocity = vol };
    mixer_submit(&cmd);
    ESP_LOGI(TAG, "ENC2 vol=%u", (unsigned)vol);
}

static void preview_k1(void)
{
    MixCmd cmd = {
        .type = MIX_CMD_NOTE_ON,
        .note_id = 0,
        .velocity = 127,
    };
    mixer_submit(&cmd);
}

static void encoder_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_EC11_A) | (1ULL << PIN_EC11_B) | (1ULL << PIN_EC11_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_last_a = gpio_get_level(PIN_EC11_A);
    s_sw_down = gpio_get_level(PIN_EC11_SW);
    ESP_LOGI(TAG, "ENC2 LEFT A=%d B=%d SW=%d  rot=vol hold+rot=key click=mute long=quick",
             (int)PIN_EC11_A, (int)PIN_EC11_B, (int)PIN_EC11_SW);

    for (;;) {
        int a = gpio_get_level(PIN_EC11_A);
        int sw = gpio_get_level(PIN_EC11_SW);
        int held = (sw == 0);
        if (a != s_last_a && a == 0) {
            int b = gpio_get_level(PIN_EC11_B);
            int dir = b ? -1 : 1;
            if (held) {
                int t = mixer_transpose() + dir;
                mixer_set_transpose(t);
                preview_k1();
                s_held_turned = 1;
                ESP_LOGI(TAG, "ENC2 key=%s (%d)", mixer_transpose_name(), mixer_transpose());
            } else {
                int vol = (int)mixer_master_ui();
                vol += dir;
                if (vol < 0) {
                    vol = 0;
                }
                if (vol > MIXER_MASTER_UI_MAX) {
                    vol = MIXER_MASTER_UI_MAX;
                }
                if (vol > 0) {
                    s_vol_before_mute = (uint8_t)vol;
                }
                set_master((uint8_t)vol);
            }
        }
        s_last_a = a;

        if (s_sw_down == 1 && sw == 0) {
            s_sw_at = xTaskGetTickCount();
            s_held_turned = 0;
        } else if (s_sw_down == 0 && sw == 1) {
            TickType_t held_ms = xTaskGetTickCount() - s_sw_at;
            if (!s_held_turned) {
                if (held_ms >= pdMS_TO_TICKS(700)) {
                    ui_tft_quick_panel_toggle();
                    ESP_LOGI(TAG, "ENC2 long → quick panel");
                } else {
                    uint8_t now = mixer_master_ui();
                    if (now == 0) {
                        set_master(s_vol_before_mute ? s_vol_before_mute : MIXER_MASTER_UI_DEFAULT);
                    } else {
                        s_vol_before_mute = now;
                        set_master(0);
                    }
                }
            }
        }
        s_sw_down = sw;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t encoder_start(void)
{
    xTaskCreatePinnedToCore(encoder_task, "enc2", 3072, NULL, 4, NULL, 0);
    return ESP_OK;
}
