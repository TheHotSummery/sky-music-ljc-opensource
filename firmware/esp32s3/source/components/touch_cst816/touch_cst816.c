#include "touch_cst816.h"

#include "board_pins.h"
#include "enc_fn.h"
#include "key_link.h"
#include "mixer.h"
#include "ui_tft.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "touch";

#define REG_GESTURE   0x01u
#define REG_FINGER    0x02u
#define REG_CHIP_ID   0xA7u
#define REG_PROJ_ID   0xA8u
#define REG_FW_VER    0xA9u
#define REG_IRQ_CTL   0xFAu
#define REG_DIS_SLEEP 0xFEu

#define CHIP_CST816D  0xB6u
#define CHIP_CST816S  0xB4u
#define CHIP_CST816T  0xB5u

static i2c_master_dev_handle_t s_dev;
static int s_ok;
static uint8_t s_chip;
static uint8_t s_addr = TOUCH_I2C_ADDR7;
static int s_ui_nav = 1;
static int s_dbg;
static uint8_t s_last_gest;
static uint8_t s_was_pressed;
static int16_t s_down_x, s_down_y;
static TickType_t s_down_at;
static int s_boot_logs = 12; /* first events always logged for bring-up */

static esp_err_t tp_wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 200);
}

static esp_err_t tp_rd(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 200);
}

/* Panel native portrait ~240x284 → UI landscape 284x240 (rot=1). */
static void map_to_ui(uint16_t rx, uint16_t ry, int16_t *ux, int16_t *uy)
{
    int x = (int)ry;
    int y = (int)TFT_HEIGHT - 1 - (int)rx;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= TFT_WIDTH) {
        x = TFT_WIDTH - 1;
    }
    if (y >= TFT_HEIGHT) {
        y = TFT_HEIGHT - 1;
    }
    *ux = (int16_t)x;
    *uy = (int16_t)y;
}

bool touch_cst816_read(touch_sample_t *out)
{
    uint8_t buf[6] = {0};
    if (!s_ok || !out) {
        return false;
    }
    key_link_i2c_lock();
    esp_err_t err = tp_rd(REG_GESTURE, buf, sizeof(buf));
    key_link_i2c_unlock();
    if (err != ESP_OK) {
        return false;
    }
    out->gesture = buf[0];
    out->fingers = (uint8_t)(buf[1] & 0x0Fu);
    out->raw_x = (uint16_t)(((buf[2] & 0x0Fu) << 8) | buf[3]);
    out->raw_y = (uint16_t)(((buf[4] & 0x0Fu) << 8) | buf[5]);
    /* Prefer finger count; XH[7:6]: 0=down 1=up 2=contact (only meaningful with touch) */
    uint8_t ev = (uint8_t)(buf[2] >> 6);
    if (out->fingers > 0) {
        out->pressed = (ev == 0x01u) ? 0u : 1u;
    } else {
        out->pressed = 0;
    }
    if (out->pressed) {
        map_to_ui(out->raw_x, out->raw_y, &out->ui_x, &out->ui_y);
    } else {
        out->ui_x = -1;
        out->ui_y = -1;
    }
    return true;
}

static void vol_nudge(int delta)
{
    int v = (int)mixer_master_ui() + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    MixCmd c = {.type = MIX_CMD_SET_MASTER, .velocity = (uint8_t)v};
    mixer_submit(&c);
}

static void apply_paint(const touch_sample_t *t, int16_t last_x, int16_t last_y)
{
    if (t->pressed && t->ui_x >= 0) {
        ui_tft_paint_set_hud((int)t->raw_x, (int)t->raw_y, (int)t->ui_x, (int)t->ui_y);
        ui_tft_paint_stroke((int)t->ui_x, (int)t->ui_y, 1, 1);
    } else if (!t->pressed && s_was_pressed) {
        /* release: end stroke; prefer last contact point for HUD */
        if (last_x >= 0) {
            ui_tft_paint_set_hud((int)t->raw_x, (int)t->raw_y, (int)last_x, (int)last_y);
        }
        ui_tft_paint_stroke(0, 0, 0, 1);
    }
}

static void apply_nav(const touch_sample_t *t)
{
    if (!s_ui_nav || ui_tft_paint_active() || ui_tft_miclab_active()) {
        s_was_pressed = t->pressed;
        return;
    }

    /* Chip gesture codes (edge). */
    if (t->gesture && t->gesture != s_last_gest) {
        switch (t->gesture) {
        case TOUCH_GEST_SINGLE:
            enc_fn_click();
            break;
        case TOUCH_GEST_DOUBLE:
        case TOUCH_GEST_LONG:
            enc_fn_long();
            break;
        case TOUCH_GEST_SWIPE_LEFT:
            enc_fn_rotate(1);
            break;
        case TOUCH_GEST_SWIPE_RIGHT:
            enc_fn_rotate(-1);
            break;
        case TOUCH_GEST_SWIPE_UP:
            vol_nudge(5);
            break;
        case TOUCH_GEST_SWIPE_DOWN:
            vol_nudge(-5);
            break;
        default:
            break;
        }
    }
    s_last_gest = t->gesture;

    /* Fallback: press/release + swipe from delta (many CST816 FW rarely emit gest). */
    if (t->pressed && !s_was_pressed) {
        s_down_x = t->ui_x;
        s_down_y = t->ui_y;
        s_down_at = xTaskGetTickCount();
    } else if (!t->pressed && s_was_pressed) {
        int dx = (int)t->ui_x - (int)s_down_x;
        int dy = (int)t->ui_y - (int)s_down_y;
        /* On release, ui may be -1 — use last down + last moved; re-read pressed path */
        if (t->ui_x < 0) {
            dx = 0;
            dy = 0;
        }
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        TickType_t held = xTaskGetTickCount() - s_down_at;
        if (adx < 12 && ady < 12) {
            if (held >= pdMS_TO_TICKS(650)) {
                enc_fn_long();
            } else {
                enc_fn_click();
            }
        } else if (adx >= ady) {
            enc_fn_rotate(dx > 0 ? -1 : 1);
        } else {
            vol_nudge(dy > 0 ? -5 : 5);
        }
    }
    if (t->pressed) {
        /* track last point for release swipe */
        if (t->ui_x >= 0) {
            /* keep down_* as start; optional live track in separate vars */
        }
    }
    s_was_pressed = t->pressed;
}

static void touch_task(void *arg)
{
    (void)arg;
    int16_t last_x = -1, last_y = -1;
    for (;;) {
        touch_sample_t t;
        int irq = gpio_get_level(PIN_TP_INT) == 0;
        if (touch_cst816_read(&t)) {
            if (t.pressed && t.ui_x >= 0) {
                last_x = t.ui_x;
                last_y = t.ui_y;
            }
            /* On release sample often has ui=-1; synthesize from last point for swipe math */
            touch_sample_t nav = t;
            if (!t.pressed && s_was_pressed && last_x >= 0) {
                nav.ui_x = last_x;
                nav.ui_y = last_y;
            }
            if (s_dbg || s_boot_logs > 0) {
                if (t.pressed || t.gesture || irq || s_was_pressed) {
                    ESP_LOGI(TAG, "gest=0x%02X fing=%u raw=%u,%u ui=%d,%d irq=%d", t.gesture,
                             t.fingers, t.raw_x, t.raw_y, (int)t.ui_x, (int)t.ui_y, irq);
                    if (s_boot_logs > 0) {
                        s_boot_logs--;
                    }
                }
            }
            if (ui_tft_paint_active()) {
                apply_paint(&t, last_x, last_y);
                s_was_pressed = t.pressed;
            } else {
                apply_nav(&nav);
            }
            if (!t.pressed) {
                last_x = last_y = -1;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void touch_cst816_dump(void)
{
    printf("touch_ok=%d addr=0x%02X chip=0x%02X ui_nav=%d dbg=%d int_gpio=%d int_lvl=%d\n", s_ok,
           s_addr, s_chip, s_ui_nav, s_dbg, (int)PIN_TP_INT, gpio_get_level(PIN_TP_INT));
    touch_sample_t t;
    if (s_ok && touch_cst816_read(&t)) {
        printf("gest=0x%02X fing=%u raw=%u,%u ui=%d,%d pressed=%u\n", t.gesture, t.fingers,
               t.raw_x, t.raw_y, (int)t.ui_x, (int)t.ui_y, t.pressed);
    }
}

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = key_link_i2c_bus();
    if (!bus) {
        printf("i2c bus=null\n");
        return;
    }
    printf("i2c_scan:");
    key_link_i2c_lock();
    for (uint8_t a = 1; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            printf(" 0x%02X", a);
        }
    }
    key_link_i2c_unlock();
    printf("\n");
}

bool touch_cst816_handle_line(const char *line)
{
    if (!line) {
        return false;
    }
    if (strcmp(line, "touch") == 0) {
        touch_cst816_dump();
        return true;
    }
    if (strcmp(line, "touch scan") == 0 || strcmp(line, "i2c scan") == 0) {
        i2c_scan();
        return true;
    }
    if (strcmp(line, "touch dbg") == 0 || strcmp(line, "touch dbg on") == 0) {
        s_dbg = 1;
        printf("touch dbg=1\n");
        return true;
    }
    if (strcmp(line, "touch dbg off") == 0) {
        s_dbg = 0;
        printf("touch dbg=0\n");
        return true;
    }
    if (strcmp(line, "touch ui on") == 0) {
        s_ui_nav = 1;
        printf("touch ui_nav=1\n");
        return true;
    }
    if (strcmp(line, "touch ui off") == 0) {
        s_ui_nav = 0;
        printf("touch ui_nav=0\n");
        return true;
    }
    if (strcmp(line, "touch paint") == 0) {
        ui_tft_paint_enter();
        return true;
    }
    if (strcmp(line, "touch paint off") == 0) {
        ui_tft_paint_exit();
        return true;
    }
    if (strcmp(line, "touch rst") == 0) {
        /* Shared with TFT_RES — blanks LCD until reboot / tft re-init */
        printf("WARNING: pulses TFT_RES (LCD will go black)\n");
        gpio_set_level(PIN_TFT_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_TFT_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        i2c_scan();
        return true;
    }
    return false;
}

int touch_cst816_ok(void)
{
    return s_ok;
}

uint8_t touch_cst816_chip_id(void)
{
    return s_chip;
}

static esp_err_t try_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (i2c_master_probe(bus, addr, 80) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_dev) {
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = KEY_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add 0x%02X: %s", addr, esp_err_to_name(err));
        return err;
    }
    uint8_t id = 0;
    err = tp_rd(REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read id @0x%02X: %s", addr, esp_err_to_name(err));
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    s_addr = addr;
    s_chip = id;
    return ESP_OK;
}

esp_err_t touch_cst816_start(void)
{
    i2c_master_bus_handle_t bus = key_link_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not ready (start key_link first)");
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    /*
     * Do NOT pulse TFT_RES here: FPC pin4 ties TP-RST to display reset.
     * Pulsing blanks the already-running ST7789 (black screen).
     * TFT init already released RES; give CST816 time after that.
     */
    vTaskDelay(pdMS_TO_TICKS(100));

    static const uint8_t try_addrs[] = {0x15u, 0x1Au, 0x38u, 0x2Eu};
    esp_err_t err = ESP_ERR_NOT_FOUND;
    key_link_i2c_lock();
    for (size_t i = 0; i < sizeof(try_addrs); i++) {
        err = try_addr(bus, try_addrs[i]);
        if (err == ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        (void)tp_wr(REG_DIS_SLEEP, 0xFFu);
        (void)tp_wr(REG_IRQ_CTL, 0x70u); /* touch + change + motion */
        uint8_t proj = 0, fw = 0;
        (void)tp_rd(REG_PROJ_ID, &proj, 1);
        (void)tp_rd(REG_FW_VER, &fw, 1);
        ESP_LOGI(TAG, "CST816 ok addr=0x%02X id=0x%02X proj=0x%02X fw=0x%02X INT=GPIO%d", s_addr,
                 s_chip, proj, fw, (int)PIN_TP_INT);
        s_ok = 1;
    } else {
        ESP_LOGE(TAG, "CST816 absent on I2C (only expect 0x28+0x62+touch). Check FPC seat / TP-VDD");
        printf("i2c_scan:");
        for (uint8_t a = 1; a < 0x78; a++) {
            if (i2c_master_probe(bus, a, 40) == ESP_OK) {
                printf(" 0x%02X", a);
            }
        }
        printf("\n");
        printf("note: touch rst shares TFT_RES — use 'touch rst' only for HW debug (will blank LCD)\n");
    }
    key_link_i2c_unlock();

    if (!s_ok) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
