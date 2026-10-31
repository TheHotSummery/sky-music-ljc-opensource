#include "rgb_led.h"
#include "board_pins.h"
#include "key_link.h"
#include "mixer.h"
#include "led_strip_encoder.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "rgb";
#define RMT_HZ 10000000
#define BRIGHT 22 /* 0-255, keep low */
#define LED_DT_MS 20
#define LED_FADE_MS 800 /* visual envelope, not PCM length */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint8_t s_grb[WS2812_COUNT * 3];
static volatile uint16_t s_hint_hold;
static volatile uint16_t s_hint_flash;

void rgb_led_hint(uint16_t hold_mask, uint16_t flash_mask)
{
    s_hint_hold = hold_mask;
    s_hint_flash = flash_mask;
}

static void hsv(uint32_t h, uint32_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint32_t max = v;
    uint32_t min = 0;
    uint32_t i = h / 60;
    uint32_t adj = (max - min) * (h % 60) / 60;
    uint32_t rr = 0, gg = 0, bb = 0;
    switch (i) {
    case 0: rr = max; gg = min + adj; bb = min; break;
    case 1: rr = max - adj; gg = max; bb = min; break;
    case 2: rr = min; gg = max; bb = min + adj; break;
    case 3: rr = min; gg = max - adj; bb = max; break;
    case 4: rr = min + adj; gg = min; bb = max; break;
    default: rr = max; gg = min; bb = max - adj; break;
    }
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
}

static void flush(void)
{
    rmt_transmit_config_t tx = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_enc, s_grb, sizeof(s_grb), &tx) == ESP_OK) {
        /* Short wait: under CPU load a 50ms block made press→light feel sticky. */
        rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(8));
    }
}

static void led_task(void *arg)
{
    (void)arg;
    uint8_t layers[AUDIO_BANK_WAV_COUNT];
    uint8_t remain[AUDIO_BANK_WAV_COUNT];
    uint8_t gen[AUDIO_BANK_WAV_COUNT];
    uint8_t last_gen[AUDIO_BANK_WAV_COUNT];
    uint8_t env[AUDIO_BANK_WAV_COUNT];
    memset(last_gen, 0, sizeof(last_gen));
    memset(env, 0, sizeof(env));
    const uint32_t step = (100u * LED_DT_MS + LED_FADE_MS / 2) / LED_FADE_MS;
    for (;;) {
        mixer_hud_keys(layers, remain);
        mixer_hud_gens(gen);
        (void)remain;
        memset(s_grb, 0, sizeof(s_grb));
        /* Low-voltage rail protection: cap WS2812 peak current when battery drops. */
        uint32_t br = BRIGHT;
        if (key_link_bat_ok()) {
            int lvl = key_link_bat_level();
            uint32_t mv = key_link_bat_mv();
            if (lvl >= 2 || (mv > 0 && mv < 3420)) {
                br = 8;
            } else if (lvl == 1 || (mv > 0 && mv < 3620)) {
                br = 14;
            }
        }
        uint16_t hold = s_hint_hold;
        uint16_t flash = s_hint_flash;
        if (hold || flash) {
            int blink = (xTaskGetTickCount() / pdMS_TO_TICKS(120)) & 1;
            for (int k = 0; k < AUDIO_BANK_WAV_COUNT && k < WS2812_COUNT; ++k) {
                uint16_t bit = (uint16_t)(1u << k);
                if (flash & bit) {
                    if (blink) {
                        s_grb[k * 3 + 0] = 0;
                        s_grb[k * 3 + 1] = (uint8_t)br;
                        s_grb[k * 3 + 2] = 0;
                    }
                } else if (hold & bit) {
                    s_grb[k * 3 + 0] = (uint8_t)br;
                    s_grb[k * 3 + 1] = (uint8_t)br;
                    s_grb[k * 3 + 2] = (uint8_t)(br / 3);
                }
            }
            flush();
            vTaskDelay(pdMS_TO_TICKS(LED_DT_MS));
            continue;
        }
        for (int k = 0; k < AUDIO_BANK_WAV_COUNT && k < WS2812_COUNT; ++k) {
            if (gen[k] != last_gen[k]) {
                last_gen[k] = gen[k];
                env[k] = 100; /* retrigger to peak so rolls are visible */
            } else if (env[k] > step) {
                env[k] = (uint8_t)(env[k] - step);
            } else {
                env[k] = 0;
            }
            if (env[k] == 0) {
                continue;
            }
            uint32_t v = br * env[k] / 100;
            if (layers[k] > 1) {
                v = (v * 5) / 4;
            }
            if (v > br + 12) {
                v = br + 12;
            }
            uint8_t r, g, b;
            hsv((uint32_t)k * 24, v, &r, &g, &b);
            s_grb[k * 3 + 0] = g;
            s_grb[k * 3 + 1] = r;
            s_grb[k * 3 + 2] = b;
        }
        flush();
        vTaskDelay(pdMS_TO_TICKS(LED_DT_MS));
    }
}

esp_err_t rgb_led_start(void)
{
    rmt_tx_channel_config_t tx = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_WS2812,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt tx: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_encoder_config_t enc = { .resolution = RMT_HZ };
    err = rmt_new_led_strip_encoder(&enc, &s_enc);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(rmt_enable(s_chan));
    /* Above UI(4) so key→LED stays snappy even if TFT is busy; below key_link(9). */
    xTaskCreatePinnedToCore(led_task, "ws2812", 3072, NULL, 6, NULL, 0);
    ESP_LOGI(TAG, "WS2812 x%d GPIO%d bright=%d fade=%dms", WS2812_COUNT, (int)PIN_WS2812, BRIGHT,
             LED_FADE_MS);
    return ESP_OK;
}
