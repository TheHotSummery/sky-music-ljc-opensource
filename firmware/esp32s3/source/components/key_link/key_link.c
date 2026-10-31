#include "key_link.h"

#include "board_pins.h"
#include "enc_fn.h"
#include "ble_link.h"
#include "kb_regs.h"
#include "mixer.h"
#include "nk_out.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "key_link";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_i2c_mu;
static volatile int s_ok;
static volatile uint16_t s_mask;
static uint16_t s_last_mask;
static int16_t s_enc_pos_last;
static int s_enc_pos_ok;
static int s_enc_acc;
static TickType_t s_enc_acc_at;
static int s_pend_click;
static int s_pend_long;
static int s_sw_held;
static int s_sw_turned;
static int s_enc_is_rate;
static TickType_t s_sw_at;
static i2c_master_dev_handle_t s_cw;
static int s_pwr_div;
static int s_bat_ok;
static uint32_t s_bat_mv;
static uint8_t s_bat_soc;       /* filtered UI % */
static uint8_t s_bat_soc_chip;  /* CW2015 raw % */
static int s_bat_alrt = 1;
static int s_bat_level; /* 0 ok, 1 low, 2 critical */
static int s_bat_crit_done;
static uint8_t s_chg_raw;
static uint8_t s_chg_flags;
static uint8_t s_soc_win[10];
static int s_soc_win_n;
static int s_soc_win_i;
static int s_soc_inited;

#define CW2015_ADDR7   0x62u
#define CW_REG_VCELL   0x02u
#define CW_REG_SOC     0x04u
#define CW_REG_MODE    0x0Au

/* Board-specific discharge map (VCELL mV → %). Tuned for MP1605 + 3.0V floor. */
static uint8_t map_mv_to_soc(uint32_t mv)
{
    static const struct {
        uint32_t mv;
        uint8_t pct;
    } pts[] = {
        {4200, 100}, {4150, 100}, {4000, 80}, {3800, 50},
        {3500, 20},  {3300, 8},   {3100, 1},  {3000, 0},
    };
    if (mv >= pts[0].mv) {
        return 100;
    }
    if (mv <= pts[sizeof(pts) / sizeof(pts[0]) - 1].mv) {
        return 0;
    }
    for (size_t i = 0; i + 1 < sizeof(pts) / sizeof(pts[0]); ++i) {
        if (mv <= pts[i].mv && mv >= pts[i + 1].mv) {
            uint32_t span = pts[i].mv - pts[i + 1].mv;
            if (span == 0) {
                return pts[i].pct;
            }
            uint32_t t = pts[i].mv - mv;
            int dp = (int)pts[i].pct - (int)pts[i + 1].pct;
            return (uint8_t)((int)pts[i].pct - (int)((t * (uint32_t)dp) / span));
        }
    }
    return 50;
}

static uint8_t soc_window_avg(uint8_t sample)
{
    s_soc_win[s_soc_win_i] = sample;
    s_soc_win_i = (s_soc_win_i + 1) % 10;
    if (s_soc_win_n < 10) {
        s_soc_win_n++;
    }
    unsigned sum = 0;
    for (int i = 0; i < s_soc_win_n; ++i) {
        sum += s_soc_win[i];
    }
    return (uint8_t)(sum / (unsigned)s_soc_win_n);
}

static void i2c_lock(void)
{
    if (s_i2c_mu) {
        xSemaphoreTake(s_i2c_mu, portMAX_DELAY);
    }
}

static void i2c_unlock(void)
{
    if (s_i2c_mu) {
        xSemaphoreGive(s_i2c_mu);
    }
}

void key_link_i2c_lock(void)
{
    i2c_lock();
}

void key_link_i2c_unlock(void)
{
    i2c_unlock();
}

i2c_master_bus_handle_t key_link_i2c_bus(void)
{
    return s_bus;
}

static esp_err_t kb_read_u8(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, 200);
}

static esp_err_t kb_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, 200);
}

/* Caller must hold i2c_lock when invoking unlocked helpers. */
static esp_err_t xsmt_set_unlocked(int unmute)
{
    if (!s_ok || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t ctrl = 0;
    esp_err_t err = kb_read_u8(KB_REG_CTRL, &ctrl);
    if (err != ESP_OK) {
        return err;
    }
    ctrl |= KB_CTRL_INV_MATRIX;
    if (unmute) {
        ctrl = (uint8_t)(ctrl & ~KB_CTRL_XSMT_MUTE);
    } else {
        ctrl = (uint8_t)(ctrl | KB_CTRL_XSMT_MUTE);
    }
    return kb_write(KB_REG_CTRL, ctrl);
}

static int xsmt_muted_unlocked(void)
{
    if (!s_ok || !s_dev) {
        return -1;
    }
    uint8_t ctrl = 0;
    if (kb_read_u8(KB_REG_CTRL, &ctrl) != ESP_OK) {
        return -1;
    }
    return (ctrl & KB_CTRL_XSMT_MUTE) ? 1 : 0;
}

esp_err_t key_link_xsmt_set(int unmute)
{
    i2c_lock();
    esp_err_t err = xsmt_set_unlocked(unmute);
    i2c_unlock();
    return err;
}

int key_link_xsmt_muted(void)
{
    i2c_lock();
    int m = xsmt_muted_unlocked();
    i2c_unlock();
    return m;
}

static void bat_apply_critical(void)
{
    if (s_bat_crit_done) {
        return;
    }
    s_bat_crit_done = 1;
    MixCmd c = {.type = MIX_CMD_SET_MASTER, .velocity = 0};
    mixer_submit(&c);
    mixer_stream_stop();
    (void)xsmt_set_unlocked(0);
    ESP_LOGW(TAG, "BAT CRITICAL mv=%lu — muted", (unsigned long)s_bat_mv);
}

static void bat_clear_critical_mute(void)
{
    if (!s_bat_crit_done) {
        return;
    }
    s_bat_crit_done = 0;
    (void)xsmt_set_unlocked(1);
    ESP_LOGI(TAG, "BAT recover — XSMT unmuted (set master manually if needed)");
}

static void poll_power(void)
{
    uint8_t raw = 0, fl = 0;
    if (kb_read_u8(KB_REG_CHG_RAW, &raw) == ESP_OK) {
        s_chg_raw = raw;
    }
    if (kb_read_u8(KB_REG_CHG_FLAGS, &fl) == ESP_OK) {
        s_chg_flags = fl;
    }
    s_bat_alrt = gpio_get_level(PIN_BAT_ALRT);
    if (!s_cw) {
        s_bat_ok = 0;
        return;
    }
    uint8_t vh = 0, vl = 0, sh = 0, sl = 0;
    uint8_t r = CW_REG_VCELL;
    if (i2c_master_transmit_receive(s_cw, &r, 1, &vh, 1, 200) != ESP_OK) {
        s_bat_ok = 0;
        return;
    }
    r = CW_REG_VCELL + 1;
    if (i2c_master_transmit_receive(s_cw, &r, 1, &vl, 1, 200) != ESP_OK) {
        s_bat_ok = 0;
        return;
    }
    r = CW_REG_SOC;
    if (i2c_master_transmit_receive(s_cw, &r, 1, &sh, 1, 200) != ESP_OK) {
        s_bat_ok = 0;
        return;
    }
    r = CW_REG_SOC + 1;
    (void)i2c_master_transmit_receive(s_cw, &r, 1, &sl, 1, 200);
    uint16_t raw_v = ((uint16_t)vh << 8) | vl;
    s_bat_mv = ((uint32_t)raw_v * 305u) / 1000u;
    s_bat_soc_chip = sh;
    s_bat_ok = 1;

    /*
     * Soft SOC: map VCELL (not chip default curve), average, then
     * TP4056 STDBY/CHRG override + one-way discharge.
     */
    uint8_t mapped = map_mv_to_soc(s_bat_mv);
    uint8_t avg = soc_window_avg(mapped);
    int charging = (s_chg_flags & KB_CHG_FLAG_CHARGING) != 0;
    int full = (s_chg_flags & KB_CHG_FLAG_FULL) != 0;

    if (full) {
        s_bat_soc = 100;
    } else if (charging) {
        if (avg > 99) {
            avg = 99;
        }
        if (!s_soc_inited || avg >= s_bat_soc) {
            s_bat_soc = avg;
        } else if (s_bat_soc > 0 && avg + 2 < s_bat_soc) {
            /* rare: allow slow drop if mapped falls a lot while “charging” noise */
            s_bat_soc--;
        }
        if (s_bat_soc > 99) {
            s_bat_soc = 99;
        }
    } else {
        /* Battery: display may only fall or hold (never climb after load sag recovery). */
        if (!s_soc_inited) {
            s_bat_soc = avg;
        } else if (avg < s_bat_soc) {
            s_bat_soc = avg;
        }
    }
    s_soc_inited = 1;

    if (s_bat_mv <= 3050u || s_bat_alrt == 0) {
        s_bat_level = 2;
        if (s_bat_soc > 1) {
            s_bat_soc = 1;
        }
        bat_apply_critical();
    } else if (s_bat_mv <= 3500u || s_bat_soc <= 20) {
        s_bat_level = 1;
        bat_clear_critical_mute();
    } else {
        s_bat_level = 0;
        bat_clear_critical_mute();
    }

    ble_link_set_battery(s_bat_soc);
}

const char *key_link_chg_name(void)
{
    if (s_chg_flags & KB_CHG_FLAG_CHARGING) {
        return "充电";
    }
    if (s_chg_flags & KB_CHG_FLAG_FULL) {
        return "满电";
    }
    if (s_chg_flags & KB_CHG_FLAG_VBUS) {
        return "USB";
    }
    return "电池";
}

int key_link_bat_ok(void)
{
    return s_bat_ok;
}

uint32_t key_link_bat_mv(void)
{
    return s_bat_mv;
}

uint8_t key_link_bat_soc(void)
{
    return s_bat_soc;
}

uint8_t key_link_bat_soc_chip(void)
{
    return s_bat_soc_chip;
}

uint8_t key_link_chg_flags(void)
{
    return s_chg_flags;
}

int key_link_bat_alrt(void)
{
    return s_bat_alrt;
}

int key_link_bat_level(void)
{
    return s_bat_level;
}

void key_link_dump_power(void)
{
    const char *chg = "idle";
    if (s_chg_flags & KB_CHG_FLAG_CHARGING) {
        chg = "charging";
    } else if (s_chg_flags & KB_CHG_FLAG_FULL) {
        chg = "full";
    } else if (s_chg_flags & KB_CHG_FLAG_VBUS) {
        chg = "vbus";
    }
    nk_printf("bat_ok=%d\n", s_bat_ok);
    nk_printf("bat_mv=%lu\n", (unsigned long)s_bat_mv);
    nk_printf("bat_soc=%u\n", (unsigned)s_bat_soc);
    nk_printf("bat_soc_chip=%u\n", (unsigned)s_bat_soc_chip);
    nk_printf("bat_level=%d\n", s_bat_level);
    nk_printf("bat_alrt=%d\n", s_bat_alrt);
    nk_printf("chg=%s\n", chg);
    nk_printf("chg_flags=0x%02X\n", s_chg_flags);
    nk_printf("chg_raw=0x%02X\n", s_chg_raw);
    /* dump_power may run without lock; use public reader */
    nk_printf("xsmt_muted=%d\n", key_link_xsmt_muted());
}

/* Two separate 1-byte reads: CH32 EVT_DATA phase is key then state.
 * A 2-byte burst can auto-increment onto CHG_RAW and desync the FIFO. */
static esp_err_t kb_pop(uint8_t *key, uint8_t *st)
{
    uint8_t reg = KB_REG_EVT_DATA;
    uint8_t k = 0xFF, s = 0xFF;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, &k, 1, 200);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &s, 1, 200);
    if (err != ESP_OK) {
        return err;
    }
    *key = k;
    *st = s;
    return ESP_OK;
}

esp_err_t key_link_whoami(uint8_t *who, uint8_t *ver)
{
    uint8_t w = 0, v = 0;
    i2c_lock();
    esp_err_t e1 = kb_read_u8(KB_REG_WHOAMI, &w);
    esp_err_t e2 = kb_read_u8(KB_REG_VERSION, &v);
    i2c_unlock();
    if (e1 != ESP_OK || e2 != ESP_OK) {
        return ESP_FAIL;
    }
    if (who) {
        *who = w;
    }
    if (ver) {
        *ver = v;
    }
    return ESP_OK;
}

uint16_t key_link_keymask(void)
{
    return s_mask;
}

int key_link_ok(void)
{
    return s_ok;
}

static void refresh_mask(void)
{
    uint8_t kl = 0, kh = 0;
    if (kb_read_u8(KB_REG_KEYMASK_L, &kl) == ESP_OK &&
        kb_read_u8(KB_REG_KEYMASK_H, &kh) == ESP_OK) {
        s_mask = (uint16_t)kl | ((uint16_t)kh << 8);
    }
}

/* CH32 I2C often extra-clocks EVT_DATA empty (0xFF). KEYMASK is trustworthy. */
static void emit_mask_edges(void)
{
    uint16_t now = s_mask;
    uint16_t changed = (uint16_t)(now ^ s_last_mask);
    if (!changed) {
        return;
    }
    for (uint8_t i = 0; i < KB_KEY_COUNT; i++) {
        if ((changed & (1u << i)) == 0) {
            continue;
        }
        int down = (now & (1u << i)) != 0;
        if (i == KB_KEY_ENC_SW) {
            if (down) {
                s_sw_held = 1;
                s_sw_turned = 0;
                s_sw_at = xTaskGetTickCount();
            } else if (s_sw_held) {
                s_sw_held = 0;
                TickType_t held = xTaskGetTickCount() - s_sw_at;
                if (s_sw_turned) {
                    s_sw_turned = 0;
                } else if (held >= pdMS_TO_TICKS(700)) {
                    s_pend_long++;
                } else {
                    s_pend_click++;
                }
            }
            ESP_LOGI(TAG, "EVT ENC_SW %s  mask=0x%04X", down ? "DOWN" : "UP", now);
        } else {
            MixCmd cmd = {
                .type = down ? MIX_CMD_NOTE_ON : MIX_CMD_NOTE_OFF,
                .note_id = i,
                .velocity = down ? 127 : 0,
            };
            mixer_submit(&cmd);
            enc_fn_on_key((int)i, down);
            ble_link_note(i, down);
            ESP_LOGI(TAG, "EVT K%u %s  mask=0x%04X", (unsigned)(i + 1), down ? "DOWN" : "UP", now);
        }
    }
    s_last_mask = now;
}

static void fifo_discard(void)
{
    for (int n = 0; n < 32; n++) {
        uint8_t cnt = 0;
        if (kb_read_u8(KB_REG_EVT_COUNT, &cnt) != ESP_OK) {
            s_ok = 0;
            return;
        }
        if (cnt == 0) {
            return;
        }
        uint8_t key = 0xFF, st = 0xFF;
        if (kb_pop(&key, &st) != ESP_OK || (key == 0xFF && st == 0xFF)) {
            return;
        }
    }
}

static void poll_encoder(void)
{
    uint8_t pl = 0, ph = 0;
    /* POS is reliable; ENC_DELTA is clear-on-read and often 0 after extra I2C clocks. */
    if (kb_read_u8(KB_REG_ENC_POS_L, &pl) != ESP_OK ||
        kb_read_u8(KB_REG_ENC_POS_H, &ph) != ESP_OK) {
        return;
    }
    int16_t pos = (int16_t)((uint16_t)pl | ((uint16_t)ph << 8));
    if (!s_enc_pos_ok) {
        s_enc_pos_last = pos;
        s_enc_pos_ok = 1;
        return;
    }
    int16_t d = (int16_t)(pos - s_enc_pos_last);
    if (d) {
        s_enc_pos_last = pos;
        s_enc_acc += (int)d;
        s_enc_acc_at = xTaskGetTickCount();
        if (s_sw_held) {
            s_sw_turned = 1;
            s_enc_is_rate = 1;
        }
    }
}

static void ensure_invert(void)
{
    uint8_t ctrl = 0;
    if (kb_read_u8(KB_REG_CTRL, &ctrl) == ESP_OK && (ctrl & KB_CTRL_INV_MATRIX)) {
        return; /* already inverted; rewriting resets scan state */
    }
    /* Preserve XSMT mute bit; only force INV_MATRIX on. */
    (void)kb_write(KB_REG_CTRL, (uint8_t)((ctrl & ~KB_CTRL_INV_MATRIX) | KB_CTRL_INV_MATRIX));
}

static void key_link_task(void *arg)
{
    (void)arg;
    for (;;) {
        i2c_lock();
        if (!s_ok) {
            uint8_t who = 0, ver = 0;
            if (kb_read_u8(KB_REG_WHOAMI, &who) == ESP_OK &&
                kb_read_u8(KB_REG_VERSION, &ver) == ESP_OK &&
                who == KB_WHOAMI_VALUE) {
                s_ok = 1;
                ensure_invert();
                ESP_LOGI(TAG, "CH32 back WHOAMI=0x%02X VER=0x%02X", who, ver);
            }
            i2c_unlock();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        refresh_mask();
        emit_mask_edges();
        poll_encoder();
        if (++s_pwr_div >= 100) {
            s_pwr_div = 0;
            poll_power();
        }
        if (gpio_get_level(PIN_INT) == 0) {
            fifo_discard();
        }
        int click = s_pend_click;
        int lng = s_pend_long;
        int skip = 0;
        /* Flush sooner when spinning fast so batches aren't delayed 90ms. */
        {
            TickType_t idle = xTaskGetTickCount() - s_enc_acc_at;
            int flush_ms = (s_enc_acc > 2 || s_enc_acc < -2) ? 20 : 35;
            if (s_enc_acc && idle >= pdMS_TO_TICKS(flush_ms)) {
                skip = s_enc_acc;
                s_enc_acc = 0;
            }
        }
        s_pend_click = s_pend_long = 0;
        i2c_unlock();
        if (skip) {
            if (s_enc_is_rate) {
                enc_fn_held_rotate(skip);
                s_enc_is_rate = 0;
            } else {
                enc_fn_rotate(skip);
            }
        }
        while (lng-- > 0) {
            enc_fn_long();
        }
        while (click-- > 0) {
            enc_fn_click();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t key_link_start(void)
{
    s_i2c_mu = xSemaphoreCreateMutex();
    if (!s_i2c_mu) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_INT) | (1ULL << PIN_BAT_ALRT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = KEY_I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_I2C_ADDR_7BIT,
        .scl_speed_hz = KEY_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c dev: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t cw_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CW2015_ADDR7,
        .scl_speed_hz = KEY_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &cw_cfg, &s_cw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CW2015 add: %s", esp_err_to_name(err));
        s_cw = NULL;
    }

    uint8_t who = 0, ver = 0;
    i2c_lock();
    if (kb_read_u8(KB_REG_WHOAMI, &who) == ESP_OK &&
        kb_read_u8(KB_REG_VERSION, &ver) == ESP_OK &&
        who == KB_WHOAMI_VALUE) {
        s_ok = 1;
        ensure_invert();
        ESP_LOGI(TAG, "CH32 WHOAMI=0x%02X VER=0x%02X (expect 0xCB / >=0x03) MATCH", who, ver);
    } else {
        ESP_LOGE(TAG, "CH32 I2C fail who=0x%02X (check 3V3, firmware, pullups)", who);
    }
    if (s_cw) {
        uint8_t wake[2] = {CW_REG_MODE, 0x00};
        (void)i2c_master_transmit(s_cw, wake, 2, 200);
    }
    poll_power();
    /* Boot: ensure DAC not left muted by a previous critical latch / soft write. */
    if (s_bat_level < 2) {
        (void)xsmt_set_unlocked(1);
    }
    int xsmt_m = xsmt_muted_unlocked();
    i2c_unlock();
    ESP_LOGI(TAG, "power bat_ok=%d mv=%lu soc=%u%% alrt=%d chg_flags=0x%02X xsmt_muted=%d",
             s_bat_ok, (unsigned long)s_bat_mv, (unsigned)s_bat_soc, s_bat_alrt, s_chg_flags, xsmt_m);

    if (xTaskCreatePinnedToCore(key_link_task, "key_link", 4096, NULL, 9, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2C SDA=%d SCL=%d INT=%d addr=0x%02X  S3 keys->mixer",
             (int)PIN_I2C_SDA, (int)PIN_I2C_SCL, (int)PIN_INT, KB_I2C_ADDR_7BIT);
    return ESP_OK;
}
