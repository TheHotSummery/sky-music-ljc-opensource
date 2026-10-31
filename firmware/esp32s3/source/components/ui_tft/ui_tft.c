#include "ui_tft.h"
#include "ui_bg.h"
#include "ui_theme.h"

#include "audio_bank.h"
#include "board_pins.h"
#include "drill.h"
#include "enc_fn.h"
#include "font_cn.h"
#include "kb_regs.h"
#include "key_link.h"
#include "mic_i2s.h"
#include "mixer.h"
#include "score_play.h"
#include "sd_music.h"
#include "usb_ident.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tft";

/*
 * ST7789P3 240x284 landscape: V2 touch FPC uses rot=1; old non-touch was rot=3.
 * UI OS Phase 0: splash → Home dashboard → mode pages (see docs/UI_OS.md).
 * Serial: tft home | tft next | tft rot | tft gap | tft inv | tft test | tft hud
 */

#define COL(r, g, b) (uint16_t)(((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Debug bring-up only (not theme). */
#define C_RED   COL(255, 0, 0)
#define C_GREEN COL(0, 255, 0)
#define C_BLUE  COL(0, 80, 255)
#define C_WHITE COL(255, 255, 255)

/* Thinner strips → less permanent DMA (~18KB saved vs 32). Need free DMA for SDSPI bounce. */
#define STRIP_H  16
#define FB_MAX   (TFT_PHYS_W * TFT_PHYS_H)

/* Skeleton: STATUS / MAIN / PARAM inside round-bezel safe inset. */
#define L_SAFE       TFT_SAFE
#define L_STATUS_Y0  (L_SAFE)
#define L_STATUS_Y1  46
#define L_MAIN_Y0    52
#define L_MAIN_Y1    168
#define L_PARAM_Y0   176
#define L_PARAM_Y1   (TFT_HEIGHT - L_SAFE)
#define L_LINE_H     FONT_CN_H
#define L_FOCUS_BOX_H 36

#define SPLASH_MS    1600

typedef enum {
    UI_PAGE_SPLASH = 0,
    UI_PAGE_HOME,
    UI_PAGE_LIBRARY,
    UI_PAGE_MODE, /* follows enc_fn five modes */
    UI_PAGE_DEBUG,
    UI_PAGE_PAINT,  /* touch XY bring-up */
    UI_PAGE_MICLAB, /* mic record/play bring-up */
} ui_page_t;

#define LIB_N 5
static const char *const s_lib_label[LIB_N] = {"乐器", "音乐", "曲谱", "跟练", "录制"};
static const enc_fn_mode_t s_lib_mode[LIB_N] = {
    ENC_FN_PLAY, ENC_FN_MUSIC, ENC_FN_SCORE, ENC_FN_FOLLOW, ENC_FN_RECORD,
};

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb;
static uint16_t *s_strip[2]; /* ping-pong: never rewrite a DMA-in-flight buffer */
static int s_strip_idx;
static SemaphoreHandle_t s_xfer_done;
static uint32_t s_lcd_frames;
static uint32_t s_lcd_fps;
static TickType_t s_fps_tick;
static int s_w = TFT_WIDTH;
static int s_h = TFT_HEIGHT;
static int s_col_off = TFT_ROW_OFFSET;
static int s_row_off = TFT_COL_OFFSET;
static int s_rot = 1;
static uint8_t s_madctl = 0x60;
static bool s_invert = true;
static int s_blk = 1;
static ui_page_t s_page = UI_PAGE_SPLASH;
static TickType_t s_splash_until;
static char s_last_mode[12];
static int s_home_list_idx = -2;
static int s_qp; /* Quick Panel overlay */
static int s_lib_sel; /* 0..LIB_N-1 */
static TickType_t s_nav_lock_until; /* ignore ENC1 nav briefly after Home */
static int s_nav_enabled;          /* 0 until enc_fn_start finished */
static TickType_t s_enc1_last_click;
static int s_pending_mode_click; /* defer enc_fn_click so double-click can cancel */
static TickType_t s_pending_mode_click_at;
static int s_pending_home_click; /* defer Home→Library so double-click opens Settings */
static TickType_t s_pending_home_click_at;
static int s_flushing_deferred; /* 1 while committing deferred MODE click */
#define DBL_MS 380
#define FALL_LOOKAHEAD_MS 2400u
#define FALL_MAX 48
/* Optional software 180° (prefer hardware rot; V2 default rot=1). */
static int s_flip180 = 0;
static volatile int s_xfer_inflight;
static volatile int s_radio_busy;
static int s_frame_ms = 50;
static char s_net[24];
static SemaphoreHandle_t s_draw_mu; /* only one task may flush LCD */

/* Paint board */
static int16_t s_paint_lx = -1, s_paint_ly = -1;
static int s_paint_raw_x, s_paint_raw_y, s_paint_ui_x, s_paint_ui_y;
static int s_paint_hud_dirty;
static TickType_t s_paint_hud_at;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t hp = pdFALSE;
    /* Only signal — do NOT clear s_xfer_inflight here (races wait_xfer early-out). */
    xSemaphoreGiveFromISR(s_xfer_done, &hp);
    return hp == pdTRUE;
}

static void gram_wipe(void);

static void lcd_cmd(int cmd, const uint8_t *data, size_t n)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, cmd, data, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cmd 0x%02x: %s", cmd, esp_err_to_name(err));
    }
}

static void set_window(int x0, int y0, int x1, int y1)
{
    uint8_t cas[4] = {
        (uint8_t)((x0 + s_col_off) >> 8),
        (uint8_t)((x0 + s_col_off) & 0xFF),
        (uint8_t)((x1 + s_col_off) >> 8),
        (uint8_t)((x1 + s_col_off) & 0xFF),
    };
    uint8_t ras[4] = {
        (uint8_t)((y0 + s_row_off) >> 8),
        (uint8_t)((y0 + s_row_off) & 0xFF),
        (uint8_t)((y1 + s_row_off) >> 8),
        (uint8_t)((y1 + s_row_off) & 0xFF),
    };
    lcd_cmd(0x2A, cas, 4);
    lcd_cmd(0x2B, ras, 4);
}

static void set_rotation(int rot)
{
    rot &= 3;
    s_rot = rot;
    switch (rot) {
    case 1: /* landscape 284x240 — V2 touch panel upright */
        s_madctl = 0x60;
        s_w = TFT_PHYS_H;
        s_h = TFT_PHYS_W;
        /* Gate gap is on the opposite end vs rot=3 → col offset 0 */
        s_col_off = TFT_COL_OFFSET; /* 0 */
        s_row_off = TFT_COL_OFFSET;
        break;
    case 2: /* portrait 180 */
        s_madctl = 0xC0;
        s_w = TFT_PHYS_W;
        s_h = TFT_PHYS_H;
        s_col_off = TFT_COL_OFFSET;
        s_row_off = TFT_ROW_OFFSET;
        break;
    case 3: /* landscape 180 (old non-touch) */
        s_madctl = 0xA0;
        s_w = TFT_PHYS_H;
        s_h = TFT_PHYS_W;
        /* MV: CASET walks former gate axis — gap at start → off=36 */
        s_col_off = TFT_ROW_OFFSET;
        s_row_off = TFT_COL_OFFSET;
        break;
    default: /* portrait 240x284 */
        s_madctl = 0x00;
        s_w = TFT_PHYS_W;
        s_h = TFT_PHYS_H;
        s_col_off = TFT_COL_OFFSET;
        s_row_off = TFT_ROW_OFFSET;
        break;
    }
    lcd_cmd(0x36, &s_madctl, 1);
    lcd_cmd(s_invert ? 0x21 : 0x20, NULL, 0);
    ESP_LOGI(TAG, "rot=%d madctl=0x%02x %dx%d off=%d,%d", s_rot, s_madctl, s_w, s_h, s_col_off,
             s_row_off);
    if (s_strip[0] && s_io) {
        gram_wipe();
    }
}

static void st7789_init(void)
{
    gpio_set_level(PIN_TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t colmod[] = {0x55}; /* RGB565 */
    lcd_cmd(0x3A, colmod, 1);

    lcd_cmd(0x21, NULL, 0);
    s_invert = true;
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t wait_xfer(TickType_t to)
{
    if (!s_xfer_inflight) {
        return ESP_OK;
    }
    /* Always Take when a transfer was started — even if DMA already finished
     * (token is waiting). Skipping Take left a stale Give and the next strip
     * could rewrite the buffer mid-DMA → horizontal flicker bars. */
    if (xSemaphoreTake(s_xfer_done, to) != pdTRUE) {
        s_xfer_inflight = 0;
        ESP_LOGW(TAG, "xfer timeout");
        return ESP_ERR_TIMEOUT;
    }
    s_xfer_inflight = 0;
    return ESP_OK;
}

static esp_err_t start_color_xfer(const uint16_t *buf, size_t nbytes)
{
    /* Drop any phantom tokens so we only wait for this transfer's callback. */
    while (xSemaphoreTake(s_xfer_done, 0) == pdTRUE) {
    }
    s_xfer_inflight = 1;
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, 0x2C, buf, nbytes);
    if (err != ESP_OK) {
        s_xfer_inflight = 0;
    }
    return err;
}

/* ST7789 SPI wants RGB565 high-byte first; FB is host uint16 (LE). */
static inline uint16_t rgb565_spi(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static void copy_strip_to(uint16_t *dst, int y, int bh)
{
    for (int row = 0; row < bh; ++row) {
        int src_y = s_flip180 ? (s_h - 1 - (y + row)) : (y + row);
        const uint16_t *src = &s_fb[src_y * s_w];
        uint16_t *d = &dst[row * s_w];
        if (!s_flip180) {
            for (int x = 0; x < s_w; ++x) {
                d[x] = rgb565_spi(src[x]);
            }
        } else {
            for (int x = 0; x < s_w; ++x) {
                d[x] = rgb565_spi(src[s_w - 1 - x]);
            }
        }
    }
}

/* Wipe full GRAM (240x320) so unused gap rows stay black. */
static void gram_wipe(void)
{
    if (!s_strip[0] || !s_io) {
        return;
    }
    int col0 = s_col_off;
    int row0 = s_row_off;
    int wipe_w = TFT_PHYS_W;
    s_col_off = 0;
    s_row_off = 0;
    memset(s_strip[0], 0, (size_t)TFT_PHYS_H * STRIP_H * sizeof(uint16_t));
    for (int y = 0; y < TFT_GRAM_H; y += STRIP_H) {
        int bh = STRIP_H;
        if (y + bh > TFT_GRAM_H) {
            bh = TFT_GRAM_H - y;
        }
        if (wait_xfer(pdMS_TO_TICKS(100)) != ESP_OK) {
            break;
        }
        set_window(0, y, wipe_w - 1, y + bh - 1);
        if (start_color_xfer(s_strip[0], (size_t)wipe_w * bh * 2) != ESP_OK) {
            break;
        }
    }
    (void)wait_xfer(pdMS_TO_TICKS(100));
    s_col_off = col0;
    s_row_off = row0;
}

static void flush_fb(void)
{
    if (!s_fb || !s_strip[0] || !s_strip[1] || !s_io) {
        return;
    }
    if (s_draw_mu) {
        xSemaphoreTake(s_draw_mu, portMAX_DELAY);
    }
    int have = 0;
    for (int y = 0; y < s_h; y += STRIP_H) {
        int bh = STRIP_H;
        if (y + bh > s_h) {
            bh = s_h - y;
        }
        uint16_t *dst = s_strip[s_strip_idx];
        /* Fill free buffer; previous DMA may still be using the other one. */
        copy_strip_to(dst, y, bh);
        if (have) {
            if (wait_xfer(pdMS_TO_TICKS(150)) != ESP_OK) {
                ESP_LOGW(TAG, "skip strip y=%d", y);
                if (s_draw_mu) {
                    xSemaphoreGive(s_draw_mu);
                }
                return;
            }
        }
        set_window(0, y, s_w - 1, y + bh - 1);
        if (start_color_xfer(dst, (size_t)s_w * bh * 2) != ESP_OK) {
            ESP_LOGW(TAG, "color xfer fail y=%d", y);
            if (s_draw_mu) {
                xSemaphoreGive(s_draw_mu);
            }
            return;
        }
        have = 1;
        s_strip_idx ^= 1;
    }
    (void)wait_xfer(pdMS_TO_TICKS(150));
    if (s_draw_mu) {
        xSemaphoreGive(s_draw_mu);
    }
}

static uint16_t lerp565(uint16_t a, uint16_t b, int t, int tmax)
{
    if (tmax <= 0) {
        return a;
    }
    if (t <= 0) {
        return a;
    }
    if (t >= tmax) {
        return b;
    }
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (br - ar) * t / tmax;
    int g = ag + (bg - ag) * t / tmax;
    int bl = ab + (bb - ab) * t / tmax;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void fb_pixel(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)s_w && (unsigned)y < (unsigned)s_h) {
        s_fb[y * s_w + x] = c;
    }
}

static void fb_fill(uint16_t color)
{
    int n = s_w * s_h;
    for (int i = 0; i < n; ++i) {
        s_fb[i] = color;
    }
}

static void fb_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            fb_pixel(xx, yy, c);
        }
    }
}

/* Prefer prebaked RGB565 in PSRAM; procedural wash only as fallback. */
static void fb_fill_theme_bg(void)
{
    if (ui_bg_ready()) {
        ui_bg_blit(s_fb, s_w, s_h);
        return;
    }
    const ui_theme_t *t = ui_theme();
    for (int y = 0; y < s_h; ++y) {
        uint16_t c = lerp565(t->bg_top, t->bg_bot, y, s_h - 1);
        uint16_t *row = &s_fb[y * s_w];
        for (int x = 0; x < s_w; ++x) {
            row[x] = c;
        }
    }
}

/* Cheap rounded rect (r≈4..8) for focus pills. */
static void fb_round_rect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (w < 2 || h < 2) {
        return;
    }
    if (r < 1) {
        fb_rect(x, y, w, h, c);
        return;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    fb_rect(x + r, y, w - 2 * r, h, c);
    fb_rect(x, y + r, w, h - 2 * r, c);
    for (int dy = 0; dy < r; ++dy) {
        int dx = 0;
        while (dx < r && (r - dx) * (r - dx) + (r - dy) * (r - dy) > r * r) {
            dx++;
        }
        int span = r - dx;
        if (span > 0) {
            fb_rect(x + dx, y + dy, span, 1, c);
            fb_rect(x + w - dx - span, y + dy, span, 1, c);
            fb_rect(x + dx, y + h - 1 - dy, span, 1, c);
            fb_rect(x + w - dx - span, y + h - 1 - dy, span, 1, c);
        }
    }
}

static void fb_diamond(int cx, int cy, int rr, uint16_t fill, uint16_t edge)
{
    if (rr < 2) {
        rr = 2;
    }
    for (int dy = -rr; dy <= rr; ++dy) {
        int hw = rr - (dy < 0 ? -dy : dy);
        for (int dx = -hw; dx <= hw; ++dx) {
            fb_pixel(cx + dx, cy + dy, fill);
        }
    }
    /* light edge */
    for (int i = 0; i <= rr; ++i) {
        fb_pixel(cx + i, cy - (rr - i), edge);
        fb_pixel(cx - i, cy - (rr - i), edge);
        fb_pixel(cx + i, cy + (rr - i), edge);
        fb_pixel(cx - i, cy + (rr - i), edge);
    }
}

static void fb_text_mw(int x, int y, const char *s, uint16_t c, int max_w)
{
    if (!s || max_w < 6) {
        return;
    }
    (void)font_cn_draw(s_fb, s_w, s_h, x, y, s, c, max_w);
}

/* Selected-row marquee: long names scroll inside [x, x+max_w). */
static char s_marquee_key[96];
static TickType_t s_marquee_since;

static void fb_text_marquee(int x, int y, const char *s, uint16_t c, int max_w)
{
    if (!s || max_w < 6) {
        return;
    }
    int tw = font_cn_width(s);
    if (tw <= max_w) {
        (void)font_cn_draw(s_fb, s_w, s_h, x, y, s, c, max_w);
        s_marquee_key[0] = 0;
        return;
    }
    if (strncmp(s_marquee_key, s, sizeof(s_marquee_key) - 1) != 0) {
        strncpy(s_marquee_key, s, sizeof(s_marquee_key) - 1);
        s_marquee_key[sizeof(s_marquee_key) - 1] = 0;
        s_marquee_since = xTaskGetTickCount();
    }
    const int gap = 28;
    const int cycle = tw + gap;
    /* Pause 700ms at start, then ~28px/s scroll, loop. */
    TickType_t elapsed = xTaskGetTickCount() - s_marquee_since;
    int ms = (int)(elapsed * portTICK_PERIOD_MS);
    int scroll = 0;
    if (ms > 700) {
        scroll = ((ms - 700) * 28 / 1000) % cycle;
    }
    (void)font_cn_draw_clip(s_fb, s_w, s_h, x - scroll, y, s, c, x, max_w);
    (void)font_cn_draw_clip(s_fb, s_w, s_h, x - scroll + cycle, y, s, c, x, max_w);
}

static void fb_text(int x, int y, const char *s, uint16_t c)
{
    int right = s_w - L_SAFE;
    int max_w = right - x;
    fb_text_mw(x, y, s, c, max_w);
}

static void fb_frame(int x, int y, int w, int h, uint16_t c)
{
    if (w < 2 || h < 2) {
        return;
    }
    fb_rect(x, y, w, 1, c);
    fb_rect(x, y + h - 1, w, 1, c);
    fb_rect(x, y, 1, h, c);
    fb_rect(x + w - 1, y, 1, h, c);
}

static void draw_bar(int x, int y, int w, int h, unsigned pct, uint16_t fill)
{
    if (w < 4 || h < 2) {
        return;
    }
    const ui_theme_t *t = ui_theme();
    fb_round_rect(x, y, w, h, h / 2, t->line);
    int fw = (int)((w - 2) * (pct > 100 ? 100u : pct) / 100u);
    if (fw > 0) {
        fb_round_rect(x + 1, y + 1, fw, h - 2, (h - 2) / 2, fill);
    }
}

static void fmt_mmss(char *buf, size_t n, uint32_t ms)
{
    unsigned sec = ms / 1000u;
    snprintf(buf, n, "%u:%02u", sec / 60u, sec % 60u);
}

static const char *mode_label_cn(const char *mode)
{
    if (strcmp(mode, "play") == 0) {
        return "弹奏";
    }
    if (strcmp(mode, "music") == 0) {
        return "音乐";
    }
    if (strcmp(mode, "score") == 0) {
        return "曲谱";
    }
    if (strcmp(mode, "follow") == 0) {
        return "跟练";
    }
    if (strcmp(mode, "record") == 0) {
        return "录制";
    }
    return mode;
}

static void draw_status_bat_vol(int x, int y, int max_w)
{
    const ui_theme_t *t = ui_theme();
    char line[48];
    uint16_t c = t->text;
    int lvl = key_link_bat_level();
    if (lvl >= 2) {
        c = C_RED;
    } else if (lvl == 1) {
        c = COL(255, 160, 40);
    }
    if (key_link_bat_ok()) {
        uint8_t fl = key_link_chg_flags();
        if (fl & KB_CHG_FLAG_FULL) {
            snprintf(line, sizeof(line), "100%%");
            c = t->ok;
        } else if (fl & KB_CHG_FLAG_CHARGING) {
            snprintf(line, sizeof(line), "%u%%+", (unsigned)key_link_bat_soc());
            c = t->ok;
        } else if (lvl >= 2) {
            snprintf(line, sizeof(line), "低电!");
        } else {
            snprintf(line, sizeof(line), "%u%%", (unsigned)key_link_bat_soc());
        }
    } else {
        snprintf(line, sizeof(line), "%s", key_link_chg_name());
    }
    fb_round_rect(x - 4, y - 2, max_w, L_LINE_H + 4, 8, t->panel2);
    fb_text_mw(x + 2, y, line, c, max_w - 6);
}

static void draw_header_pill(int x, int y, const char *title, int w)
{
    const ui_theme_t *t = ui_theme();
    fb_round_rect(x, y - 2, w, L_LINE_H + 4, 8, t->accent);
    fb_text_mw(x + 6, y, title, t->on_accent, w - 10);
}

static void draw_vol_bar(int x, int y, int w, int h)
{
    unsigned vol = mixer_master_ui();
    if (vol > 100) {
        vol = 100;
    }
    draw_bar(x, y, w, h, vol, ui_theme()->accent);
}

void ui_tft_quick_panel_toggle(void)
{
    s_qp = !s_qp;
}

static void draw_quick_panel(void)
{
    const ui_theme_t *t = ui_theme();
    const int x0 = L_SAFE + 6;
    const int y0 = L_SAFE + 24;
    const int w = s_w - 2 * L_SAFE - 12;
    const int h = s_h - 2 * L_SAFE - 32;
    char line[48];

    fb_round_rect(x0, y0, w, h, 12, t->panel);
    fb_frame(x0, y0, w, h, t->accent);
    draw_header_pill(x0 + 8, y0 + 8, "设置", 52);

    fb_text_mw(x0 + 10, y0 + 34, "音量", t->dim, 44);
    draw_vol_bar(x0 + 54, y0 + 37, w - 92, 10);
    snprintf(line, sizeof(line), "%u%%", (unsigned)mixer_master_ui());
    fb_text_mw(x0 + w - 34, y0 + 34, line, t->text, 32);

    snprintf(line, sizeof(line), "移调  %s", mixer_transpose_name());
    fb_text_mw(x0 + 10, y0 + 58, line, t->text, w - 20);

    int tempo = score_tempo_pct();
    snprintf(line, sizeof(line), "速度  %d.%dx", tempo / 100, (tempo % 100) / 10);
    fb_text_mw(x0 + 10, y0 + 80, line, tempo == 100 ? t->dim : t->accent, w - 20);

    const char *th_cn = (ui_theme_id() == UI_THEME_SKY) ? "晴空" : "夜空";
    snprintf(line, sizeof(line), "主题  %s", th_cn);
    fb_text_mw(x0 + 10, y0 + 102, line, t->accent, w - 20);
    fb_text_mw(x0 + 10, y0 + h - 22, "转旋钮1切换主题", t->dim, w - 20);
}

static void draw_brand_splash(void)
{
    const ui_theme_t *t = ui_theme();
    fb_fill_theme_bg();
    const int x0 = L_SAFE;
    const int cw = s_w - 2 * L_SAFE;
    fb_round_rect(x0 + 16, s_h / 2 - 38, cw - 32, 68, 12, t->panel);
    fb_text_mw(x0 + 32, s_h / 2 - 26, "SKY-music", t->accent, cw - 64);
    fb_text_mw(x0 + 32, s_h / 2 - 2, "光遇音乐键盘", t->dim, cw - 64);
    flush_fb();
}

/* Home: instrument dashboard — current state, not a menu. */
static void draw_page_home(void)
{
    const ui_theme_t *t = ui_theme();
    const int x0 = L_SAFE;
    const int x1 = s_w - L_SAFE;
    const int cw = x1 - x0;
    char line[96];

    fb_fill_theme_bg();

    draw_header_pill(x0, L_STATUS_Y0 + 2, "主页", 52);
    fb_text_mw(x0 + 60, L_STATUS_Y0 + 2, "SKY-music", t->dim, 100);
    draw_status_bat_vol(x1 - 52, L_STATUS_Y0 + 2, 52);

    const char *mode = enc_fn_mode_name();
    const char *bank = audio_bank_name();
    if (!bank || !bank[0]) {
        bank = "-";
    }

    int y = L_MAIN_Y0 + 2;
    /* Main instrument card */
    fb_round_rect(x0, y, cw, 50, 10, t->panel);
    snprintf(line, sizeof(line), "模式  %s", mode_label_cn(mode));
    fb_text_mw(x0 + 10, y + 6, line, t->accent, cw - 20);
    snprintf(line, sizeof(line), "音色  %s", bank);
    fb_text_mw(x0 + 10, y + 26, line, t->text, cw - 20);
    y += 56;

    unsigned pct = 0;
    uint32_t ms = 0, dur = 0;
    int mplay = 0, splay = 0, spause = 0;
    const char *track = NULL;
    sd_music_get_progress(&ms, &dur, &pct, &mplay);
    if (mplay) {
        const char *pp = sd_music_path();
        if (pp && pp[0] && pp[0] != '-') {
            const char *bn = strrchr(pp, '/');
            track = bn ? bn + 1 : pp;
        }
    } else {
        score_get_progress(&ms, &dur, &pct, &splay, &spause);
        if (splay || spause) {
            const char *pp = score_current_path();
            if (pp && pp[0] && pp[0] != '-') {
                const char *bn = strrchr(pp, '/');
                track = bn ? bn + 1 : pp;
            }
        }
    }

    /* Background activity strip (phone-like "now playing") */
    {
        drill_view_t dv;
        drill_view(&dv);
        char bg[80];
        bg[0] = 0;
        if (dv.rec == 1) {
            unsigned sec = dv.rec_ms / 1000u;
            snprintf(bg, sizeof(bg), "录音中 %u:%02u", sec / 60u, sec % 60u);
        } else if (dv.follow == 1) {
            snprintf(bg, sizeof(bg), "跟练中 %d/%d", dv.at + 1, dv.total > 0 ? dv.total : 0);
        } else if (track) {
            char t0[16];
            fmt_mmss(t0, sizeof(t0), ms);
            snprintf(bg, sizeof(bg), "%s  %s", track, t0);
        }
        if (bg[0]) {
            fb_round_rect(x0, y, cw, 44, 10, t->panel2);
            fb_text_mw(x0 + 8, y + 6, bg, t->ok, cw - 16);
            if (track && !dv.rec && dv.follow != 1) {
                draw_bar(x0 + 8, y + 28, cw - 16, 8, pct, t->ok);
            }
        } else {
            fb_round_rect(x0, y, cw, 44, 10, t->panel2);
            fb_text_mw(x0 + 10, y + 6, "状态  就绪", t->ok, cw - 20);
            int tempo = score_tempo_pct();
            const char *th_cn = (ui_theme_id() == UI_THEME_SKY) ? "晴空" : "夜空";
            snprintf(line, sizeof(line), "%s · %d.%dx · %s", mixer_transpose_name(), tempo / 100,
                     (tempo % 100) / 10, th_cn);
            fb_text_mw(x0 + 10, y + 24, line, t->dim, cw - 20);
        }
    }

    /* PARAM */
    fb_text_mw(x0, L_PARAM_Y0, "音量", t->dim, 40);
    draw_vol_bar(x0 + 42, L_PARAM_Y0 + 3, cw - 42, 10);
    fb_text_mw(x0, L_PARAM_Y0 + 18, "转旋钮或短按进入曲库", t->accent, cw);
}

/* 15-column wait-waterfall: diamond notes, group solid seps, judge line. */
static void draw_follow_waterfall(int x0, int cw, const drill_view_t *dv)
{
    const ui_theme_t *t = ui_theme();
    const int y0 = L_MAIN_Y0 + 2;
    const int y1 = L_MAIN_Y1 - 4;
    const int judge_y = y1 - 18;
    const int col_w = cw / 15;
    char line[48];

    /* soft lane ticks */
    for (int c = 0; c < 15; ++c) {
        int x = x0 + c * col_w + col_w / 2;
        for (int yy = y0; yy < judge_y; yy += 5) {
            fb_pixel(x, yy, t->gutter);
        }
    }
    /* 3×5 groups: solid at 5|6 and 10|11 */
    {
        int x_a = x0 + 5 * col_w;
        int x_b = x0 + 10 * col_w;
        for (int yy = y0; yy < judge_y; ++yy) {
            fb_pixel(x_a, yy, t->sep);
            if (x_a + 1 < x0 + cw) {
                fb_pixel(x_a + 1, yy, t->sep);
            }
            fb_pixel(x_b, yy, t->sep);
            if (x_b + 1 < x0 + cw) {
                fb_pixel(x_b + 1, yy, t->sep);
            }
        }
    }

    fb_rect(x0, judge_y, cw, 2, t->accent);

    drill_fall_t fall[FALL_MAX];
    int fn = drill_timeline(fall, FALL_MAX);
    uint32_t t0 = 0;
    if (fn > 0) {
        t0 = fall[0].t_ms;
    }
    const int span = (int)FALL_LOOKAHEAD_MS;
    const int lane_h = judge_y - y0 - 4;

    for (int i = 0; i < fn; ++i) {
        int dt = (int)(fall[i].t_ms - t0);
        if (dt < 0) {
            dt = 0;
        }
        if (dt > span && !fall[i].on_judge) {
            continue;
        }
        int col = (int)fall[i].key - 1;
        if (col < 0 || col > 14) {
            continue;
        }
        int cx = x0 + col * col_w + col_w / 2;
        int cy;
        uint16_t fill, edge;
        int rad;
        if (fall[i].on_judge) {
            cy = judge_y - 8;
            fill = t->note_cur;
            edge = t->ok;
            rad = 6;
        } else {
            int py = (dt * lane_h) / span;
            cy = judge_y - 8 - py;
            if (cy < y0 + 4) {
                continue;
            }
            fill = t->note_next;
            edge = t->accent;
            rad = 5;
        }
        fb_diamond(cx, cy, rad, fill, edge);
    }

    if (dv->total > 0) {
        snprintf(line, sizeof(line), "%d/%d 对%d错%d", dv->at + 1, dv->total, dv->hit, dv->miss);
        fb_text_mw(x0, judge_y + 4, line, t->dim, cw);
    }
}

static void draw_page_library(void)
{
    const ui_theme_t *t = ui_theme();
    const int x0 = L_SAFE;
    const int x1 = s_w - L_SAFE;
    const int cw = x1 - x0;

    fb_fill_theme_bg();
    draw_header_pill(x0, L_STATUS_Y0 + 2, "功能", 52);
    draw_status_bat_vol(x1 - 52, L_STATUS_Y0 + 2, 52);

    fb_round_rect(x0, L_MAIN_Y0 - 2, cw, L_MAIN_Y1 - L_MAIN_Y0 + 4, 10, t->panel);
    int y = L_MAIN_Y0 + 4;
    for (int i = 0; i < LIB_N; ++i) {
        int selected = (i == s_lib_sel);
        if (selected) {
            fb_round_rect(x0 + 4, y - 2, cw - 8, L_LINE_H + 5, 7, t->accent);
            fb_text_mw(x0 + 14, y, s_lib_label[i], t->on_accent, cw - 28);
        } else {
            fb_text_mw(x0 + 14, y, s_lib_label[i], t->dim, cw - 28);
        }
        y += L_LINE_H + 7;
        if (y + L_LINE_H > L_MAIN_Y1) {
            break;
        }
    }
    fb_text_mw(x0, L_PARAM_Y0 + 4, "短按进入  长按回主页", t->dim, cw);
}

static void draw_page_mode(void)
{
    const ui_theme_t *t = ui_theme();
    const int x0 = L_SAFE;
    const int x1 = s_w - L_SAFE;
    const int cw = x1 - x0;
    char line[96];

    fb_fill_theme_bg();

    const char *mode = enc_fn_mode_name();
    const char *mode_cn = mode_label_cn(mode);
    int follow_mode = strcmp(mode, "follow") == 0;
    int record_mode = strcmp(mode, "record") == 0;

    /* STATUS */
    draw_header_pill(x0, L_STATUS_Y0 + 2, mode_cn, 52);
    {
        const int rx = x0 + 60;
        int tempo = score_tempo_pct();
        snprintf(line, sizeof(line), "%s · %d.%dx", mixer_transpose_name(), tempo / 100,
                 (tempo % 100) / 10);
        fb_text_mw(rx, L_STATUS_Y0 + 2, line, tempo == 100 ? t->dim : t->accent, x1 - rx - 56);
        draw_status_bat_vol(x1 - 52, L_STATUS_Y0 + 2, 52);
    }

    int n = enc_fn_list_n();
    int idx = enc_fn_list_idx();
    const char *prev = "-";
    const char *sel = enc_fn_list_name(idx);
    const char *next = "-";
    if (n > 0 && idx >= 0) {
        prev = enc_fn_list_name((idx - 1 + n) % n);
        next = enc_fn_list_name((idx + 1) % n);
    }
    if (!sel || !sel[0]) {
        sel = "-";
    }

    int need_ok = 0;
    const char *playing_name = NULL;
    unsigned pct = 0;
    uint32_t ms = 0, dur = 0;
    int mplay = 0, splay = 0, spause = 0;
    if (strcmp(mode, "music") == 0) {
        sd_music_get_progress(&ms, &dur, &pct, &mplay);
        const char *pp = sd_music_path();
        if (mplay && pp && pp[0] && pp[0] != '-') {
            const char *bn = strrchr(pp, '/');
            playing_name = bn ? bn + 1 : pp;
        }
        if (sel[0] != '-' && (!playing_name || strcasecmp(playing_name, sel) != 0)) {
            need_ok = 1;
        }
    } else if (strcmp(mode, "score") == 0 || follow_mode) {
        score_get_progress(&ms, &dur, &pct, &splay, &spause);
        const char *pp = score_current_path();
        if ((splay || spause) && pp && pp[0] && pp[0] != '-') {
            const char *bn = strrchr(pp, '/');
            playing_name = bn ? bn + 1 : pp;
        }
        if (!follow_mode && sel[0] != '-' &&
            (!playing_name || strcasecmp(playing_name, sel) != 0)) {
            need_ok = 1;
        }
    } else if (strcmp(mode, "play") == 0) {
        if (sel[0] != '-' && strcasecmp(sel, audio_bank_name()) != 0) {
            need_ok = 1;
        }
    }

    drill_view_t dv;
    drill_view(&dv);

    /* MAIN */
    if (follow_mode && dv.follow == 1) {
        /* Wait-waterfall: 15 cols, Δt fall, current chord on judge line */
        draw_follow_waterfall(x0, cw, &dv);
    } else if (follow_mode && dv.follow == 2) {
        fb_round_rect(x0, L_MAIN_Y0 + 12, cw, 76, 10, t->panel);
        int y = L_MAIN_Y0 + 24;
        snprintf(line, sizeof(line), "练习完成");
        fb_text_mw(x0 + 12, y, line, t->ok, cw - 24);
        y += L_LINE_H + 10;
        snprintf(line, sizeof(line), "命中 %d  ·  失误 %d", dv.hit, dv.miss);
        fb_text_mw(x0 + 12, y, line, t->text, cw - 24);
    } else if (record_mode && dv.rec) {
        int box_y = L_MAIN_Y0 + 18;
        int box_h = L_FOCUS_BOX_H + 8;
        fb_round_rect(x0, box_y, cw, box_h, 10, t->panel);
        fb_frame(x0, box_y, cw, box_h, t->accent);
        unsigned sec = dv.rec_ms / 1000u;
        snprintf(line, sizeof(line), "正在录制 %u:%02u  (%d音)", sec / 60u, sec % 60u, dv.rec_n);
        fb_text_mw(x0 + 10, box_y + (box_h - L_LINE_H) / 2, line, t->accent, cw - 20);
    } else {
        /* 5-row window centered on selection inside a clean rounded panel. */
        fb_round_rect(x0, L_MAIN_Y0 - 2, cw, L_MAIN_Y1 - L_MAIN_Y0 + 4, 10, t->panel);
        const int row_h = L_LINE_H + 6;
        const int rows = 5;
        const int win0 = L_MAIN_Y0 + 2;
        int focus = rows / 2;
        for (int r = 0; r < rows; ++r) {
            int y = win0 + r * row_h;
            if (y + L_LINE_H > L_MAIN_Y1) {
                break;
            }
            if (n <= 0) {
                if (r == focus) {
                    fb_text_mw(x0 + 12, y, "暂无文件", t->dim, cw - 24);
                }
                continue;
            }
            int li = (idx - focus + r + n * 8) % n;
            const char *name = enc_fn_list_name(li);
            if (!name || !name[0]) {
                name = "-";
            }
            int selected = (r == focus);
            if (selected) {
                fb_round_rect(x0 + 4, y - 2, cw - 8, L_LINE_H + 4, 7, t->accent);
                fb_text_marquee(x0 + 10, y, name, t->on_accent, cw - 76);
                snprintf(line, sizeof(line), "%d/%d", li + 1, n);
                fb_text_mw(x0 + cw - 66, y, line, t->on_accent, 60);
            } else {
                fb_text_mw(x0 + 10, y, name, t->dim, cw - 16);
            }
        }
        (void)prev;
        (void)next;
        (void)sel;
    }

    /* PARAM: progress + contextual tip only (no idle manual text) */
    const char *tip = NULL;
    uint16_t tip_c = t->accent;
    if (follow_mode && dv.follow) {
        tip = dv.follow == 2 ? NULL : "短按停止  灯亮的键";
    } else if (follow_mode) {
        tip = "短按开始跟练";
        tip_c = t->dim;
    } else if (record_mode && dv.rec) {
        tip = "短按停止并存为新曲谱";
    } else if (record_mode) {
        if (strcmp(sel, DRILL_NEW_NAME) == 0) {
            tip = "短按开始新录音(自动存为rec曲谱)";
        } else {
            tip = "短按开始录音(覆盖所选曲谱)";
        }
        tip_c = t->dim;
    } else if (need_ok) {
        tip = "短按确认加载";
    } else if (strcmp(mode, "score") == 0 && spause) {
        tip = "已暂停  短按继续";
    } else if (playing_name) {
        snprintf(line, sizeof(line), "播放 %s", playing_name);
        tip = line; /* line lives for this draw call */
        tip_c = t->ok;
    }
    {
        const char *err = enc_fn_take_tip();
        if (err && err[0]) {
            tip = err;
            tip_c = t->warn;
        }
    }

    int y_tip = L_PARAM_Y0;
    int show_bar = 0;
    unsigned bar_pct = 0;
    if (follow_mode && dv.follow == 1 && dv.total > 0) {
        show_bar = 1;
        bar_pct = (unsigned)dv.at * 100u / (unsigned)dv.total;
    } else if ((mplay || splay || spause) &&
               (strcmp(mode, "music") == 0 || strcmp(mode, "score") == 0)) {
        show_bar = 1;
        bar_pct = pct;
    }

    if (show_bar) {
        /* "99:59/99:59" needs ~80px at current digit widths */
        const int time_w = 84;
        int bar_w = cw - time_w - 8;
        if (bar_w < 40) {
            bar_w = cw / 2;
        }
        draw_bar(x0, y_tip, bar_w, 10, bar_pct, t->ok);
        if ((mplay || splay || spause) &&
            (strcmp(mode, "music") == 0 || strcmp(mode, "score") == 0)) {
            char t0[16], t1[16];
            fmt_mmss(t0, sizeof(t0), ms);
            fmt_mmss(t1, sizeof(t1), dur);
            snprintf(line, sizeof(line), "%s/%s", t0, t1);
            fb_text_mw(x0 + bar_w + 6, y_tip - 1, line, t->text, time_w);
        }
        y_tip += 16;
    }
    if (tip && y_tip + L_LINE_H <= L_PARAM_Y1) {
        fb_text_mw(x0, y_tip, tip, tip_c, cw);
    }
}

static void resume_ui_home(void);

static void ui_sync_page_from_mode(void)
{
    const char *mode = enc_fn_mode_name();
    if (!mode) {
        return;
    }
    /* Track only — never auto-leave Home (enc_fn_start scans music/score/play). */
    strncpy(s_last_mode, mode, sizeof(s_last_mode) - 1);
    s_last_mode[sizeof(s_last_mode) - 1] = 0;
}

static void draw_ui_frame(void)
{
    if (s_page == UI_PAGE_SPLASH) {
        draw_brand_splash();
        return;
    }
    if (s_page == UI_PAGE_HOME) {
        draw_page_home();
    } else if (s_page == UI_PAGE_LIBRARY) {
        draw_page_library();
    } else if (s_page == UI_PAGE_MODE) {
        draw_page_mode();
    } else {
        return;
    }
    if (s_qp) {
        draw_quick_panel();
    }
    flush_fb();
}

static int nav_locked(void)
{
    if (!s_nav_enabled) {
        return 1;
    }
    return (int32_t)(xTaskGetTickCount() - s_nav_lock_until) < 0;
}

void ui_tft_bg_reload(void)
{
    (void)ui_bg_load_for_theme();
}

void ui_tft_nav_enable(void)
{
    s_nav_enabled = 1;
    s_nav_lock_until = xTaskGetTickCount() + pdMS_TO_TICKS(400);
    const char *mode = enc_fn_mode_name();
    if (mode) {
        strncpy(s_last_mode, mode, sizeof(s_last_mode) - 1);
        s_last_mode[sizeof(s_last_mode) - 1] = 0;
    }
    /* Stay on Home after boot scan; do not jump to MODE. */
    if (s_page == UI_PAGE_SPLASH) {
        return;
    }
    if (s_page != UI_PAGE_HOME && s_page != UI_PAGE_LIBRARY && s_page != UI_PAGE_MODE) {
        return;
    }
    if (s_page != UI_PAGE_HOME) {
        /* If boot noise already yanked us, snap back once. */
        s_page = UI_PAGE_HOME;
    }
}

bool ui_tft_on_enc1_rotate(int delta)
{
    if (nav_locked()) {
        return true; /* eat boot noise */
    }
    if (mic_lab_active()) {
        return mic_lab_on_rotate(delta);
    }
    if (s_page == UI_PAGE_PAINT) {
        return true; /* ignore rotate on paint */
    }
    /* Quick Panel open: rotate cycles skin (beauty switch) */
    if (s_qp && delta) {
        (void)ui_theme_next();
        (void)ui_bg_load_for_theme();
        return true;
    }
    if (s_page == UI_PAGE_HOME) {
        s_page = UI_PAGE_LIBRARY;
        s_lib_sel = 0;
        if (delta < 0) {
            s_lib_sel = LIB_N - 1;
        } else if (delta > 0) {
            s_lib_sel = 1 % LIB_N;
        }
        return true;
    }
    if (s_page == UI_PAGE_LIBRARY) {
        int d = enc_fn_accel_delta(delta) % LIB_N;
        s_lib_sel = (s_lib_sel + d + LIB_N * 8) % LIB_N;
        return true;
    }
    return false;
}

bool ui_tft_on_enc1_click(void)
{
    if (s_flushing_deferred) {
        return false; /* pass through to enc_fn_click body */
    }
    if (nav_locked()) {
        return true;
    }
    if (mic_lab_active()) {
        return mic_lab_on_click();
    }
    if (s_page == UI_PAGE_PAINT) {
        ui_tft_paint_clear();
        return true;
    }
    TickType_t now = xTaskGetTickCount();
    /* If Quick Panel is open, a single click closes it */
    if (s_qp) {
        s_qp = 0;
        s_enc1_last_click = 0;
        s_pending_home_click = 0;
        s_pending_mode_click = 0;
        return true;
    }
    /* Double-click on Home → Settings (Quick Panel). Home single-click is deferred
     * so the first click does NOT flash into Library. */
    if (s_page == UI_PAGE_HOME && s_enc1_last_click &&
        (now - s_enc1_last_click) <= pdMS_TO_TICKS(DBL_MS)) {
        s_enc1_last_click = 0;
        s_pending_home_click = 0;
        ui_tft_quick_panel_toggle();
        return true;
    }
    /* Double-click on MODE/LIBRARY → Home (original back gesture). */
    if ((s_page == UI_PAGE_MODE || s_page == UI_PAGE_LIBRARY) && s_enc1_last_click &&
        (now - s_enc1_last_click) <= pdMS_TO_TICKS(DBL_MS)) {
        s_enc1_last_click = 0;
        s_pending_mode_click = 0;
        s_pending_home_click = 0;
        resume_ui_home();
        return true;
    }
    s_enc1_last_click = now;

    if (s_page == UI_PAGE_HOME) {
        /* Defer enter-Library so double-click can open Settings without flashing. */
        s_pending_home_click = 1;
        s_pending_home_click_at = now;
        return true;
    }
    if (s_page == UI_PAGE_LIBRARY) {
        if (s_lib_sel < 0 || s_lib_sel >= LIB_N) {
            s_lib_sel = 0;
        }
        enc_fn_set_mode(s_lib_mode[s_lib_sel]);
        s_page = UI_PAGE_MODE;
        {
            const char *mode = enc_fn_mode_name();
            if (mode) {
                strncpy(s_last_mode, mode, sizeof(s_last_mode) - 1);
                s_last_mode[sizeof(s_last_mode) - 1] = 0;
            }
        }
        return true;
    }
    /* MODE: defer confirm so a quick second click can go Home without pausing. */
    if (s_page == UI_PAGE_MODE) {
        s_pending_mode_click = 1;
        s_pending_mode_click_at = now;
        return true;
    }
    return false;
}

bool ui_tft_on_enc1_long(void)
{
    if (nav_locked()) {
        return true;
    }
    if (mic_lab_active()) {
        return mic_lab_on_long();
    }
    if (s_page == UI_PAGE_PAINT) {
        ui_tft_paint_exit();
        return true;
    }
    if (s_page == UI_PAGE_LIBRARY || s_page == UI_PAGE_MODE) {
        resume_ui_home();
        return true;
    }
    return false;
}

static void paint_brush(int x, int y, uint16_t c)
{
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            fb_pixel(x + dx, y + dy, c);
        }
    }
}

static void paint_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    for (;;) {
        paint_brush(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 > -ady) {
            err -= ady;
            x0 += sx;
        }
        if (e2 < adx) {
            err += adx;
            y0 += sy;
        }
    }
}

static void paint_draw_base(void)
{
    const uint16_t bg = COL(8, 10, 18);
    const uint16_t grid = COL(28, 32, 48);
    const uint16_t cross = COL(60, 70, 100);
    const uint16_t lab = COL(180, 200, 255);
    fb_fill(bg);
    for (int x = 0; x < s_w; x += 28) {
        for (int y = 0; y < s_h; ++y) {
            fb_pixel(x, y, grid);
        }
    }
    for (int y = 0; y < s_h; y += 28) {
        for (int x = 0; x < s_w; ++x) {
            fb_pixel(x, y, grid);
        }
    }
    int cx = s_w / 2, cy = s_h / 2;
    for (int x = 0; x < s_w; ++x) {
        fb_pixel(x, cy, cross);
    }
    for (int y = 0; y < s_h; ++y) {
        fb_pixel(cx, y, cross);
    }
    fb_text_mw(cx - 16, 4, "TOP", lab, 40);
    fb_text_mw(cx - 28, s_h - 20, "BOTTOM", lab, 60);
    fb_text_mw(4, cy - 8, "L", lab, 16);
    fb_text_mw(s_w - 20, cy - 8, "R", lab, 16);
    fb_rect(0, 0, 12, 12, C_RED);
    fb_rect(s_w - 12, 0, 12, 12, C_GREEN);
    fb_rect(0, s_h - 12, 12, 12, C_BLUE);
    fb_rect(s_w - 12, s_h - 12, 12, 12, C_WHITE);
    fb_text_mw(4, 22, "PAINT  click=清  long=退", lab, s_w - 8);
}

static void paint_draw_hud(void)
{
    char line[48];
    fb_rect(0, s_h - 22, s_w, 22, COL(0, 0, 0));
    snprintf(line, sizeof(line), "raw %d,%d  ui %d,%d", s_paint_raw_x, s_paint_raw_y,
             s_paint_ui_x, s_paint_ui_y);
    fb_text_mw(4, s_h - 18, line, C_WHITE, s_w - 8);
}

int ui_tft_paint_active(void)
{
    return s_page == UI_PAGE_PAINT;
}

void ui_tft_paint_enter(void)
{
    if (mic_lab_active()) {
        (void)mic_lab_on_long();
    }
    s_page = UI_PAGE_PAINT;
    s_paint_lx = s_paint_ly = -1;
    s_paint_hud_dirty = 0;
    paint_draw_base();
    paint_draw_hud();
    flush_fb();
    printf("paint: finger draws; ENC1 click=clear long=exit\n");
}

void ui_tft_paint_exit(void)
{
    if (s_page != UI_PAGE_PAINT) {
        return;
    }
    s_paint_lx = s_paint_ly = -1;
    resume_ui_home();
    printf("paint off\n");
}

void ui_tft_paint_clear(void)
{
    if (s_page != UI_PAGE_PAINT) {
        return;
    }
    s_paint_lx = s_paint_ly = -1;
    paint_draw_base();
    paint_draw_hud();
    flush_fb();
}

void ui_tft_paint_set_hud(int raw_x, int raw_y, int ui_x, int ui_y)
{
    s_paint_raw_x = raw_x;
    s_paint_raw_y = raw_y;
    s_paint_ui_x = ui_x;
    s_paint_ui_y = ui_y;
    s_paint_hud_dirty = 1;
}

void ui_tft_paint_stroke(int x, int y, int down, int flush)
{
    if (s_page != UI_PAGE_PAINT || !s_fb) {
        return;
    }
    if (!down) {
        s_paint_lx = s_paint_ly = -1;
        if (flush) {
            paint_draw_hud();
            flush_fb();
            s_paint_hud_dirty = 0;
        }
        return;
    }
    const uint16_t ink = COL(0, 220, 255);
    if (s_paint_lx >= 0) {
        paint_line(s_paint_lx, s_paint_ly, x, y, ink);
    } else {
        paint_brush(x, y, ink);
    }
    s_paint_lx = (int16_t)x;
    s_paint_ly = (int16_t)y;
    if (flush) {
        TickType_t now = xTaskGetTickCount();
        if (s_paint_hud_dirty && (now - s_paint_hud_at) >= pdMS_TO_TICKS(80)) {
            paint_draw_hud();
            s_paint_hud_at = now;
            s_paint_hud_dirty = 0;
        }
        flush_fb();
    }
}

static void draw_miclab_frame(void)
{
    const ui_theme_t *t = ui_theme();
    const int x0 = L_SAFE;
    const int cw = s_w - 2 * L_SAFE;
    fb_fill_theme_bg();
    mic_lab_phase_t ph = mic_lab_phase();
    char line[48];

    if (ph == MIC_LAB_EAR) {
        fb_text_mw(x0, L_STATUS_Y0 + 2, "MIC EAR", t->accent, 100);
        fb_text_mw(x0, L_MAIN_Y0, "请插入耳机！", C_RED, cw);
        fb_text_mw(x0, L_MAIN_Y0 + 28, "喇叭耳返易啸叫", t->text, cw);
        snprintf(line, sizeof(line), "rms=%d  vol=%u", mic_i2s_rms(), (unsigned)mixer_master_ui());
        fb_text_mw(x0, L_MAIN_Y0 + 56, line, t->dim, cw);
        if (mic_ear_key_mute()) {
            fb_text_mw(x0, L_MAIN_Y0 + 84, "按键静音1s…", t->accent, cw);
        } else if (mic_ear_howl()) {
            fb_text_mw(x0, L_MAIN_Y0 + 84, "啸叫抑制中", C_RED, cw);
        } else {
            fb_text_mw(x0, L_MAIN_Y0 + 84, "实时监听中", t->ok, cw);
        }
        fb_text_mw(x0, L_PARAM_Y0, "左旋钮音量  长按退出", t->dim, cw);
        flush_fb();
        return;
    }

    fb_text_mw(x0, L_STATUS_Y0 + 2, "MIC LAB", t->accent, 100);
    int sel = mic_lab_sel();
    int len = mic_lab_len();
    const char *phs = "?";
    switch (ph) {
    case MIC_LAB_IDLE:
        phs = "就绪:短按开始录音";
        break;
    case MIC_LAB_REC:
        phs = "录音中… 短按结束";
        break;
    case MIC_LAB_READY:
        phs = "已录:旋钮选 短按执行";
        break;
    case MIC_LAB_PLAY:
        phs = "播放中";
        break;
    case MIC_LAB_PAUSE:
        phs = "已暂停";
        break;
    default:
        phs = "—";
        break;
    }
    fb_text_mw(x0, L_MAIN_Y0, phs, t->text, cw);
    snprintf(line, sizeof(line), "len=%d  pos=%d  rms=%d", len, mic_lab_play_pos(), mic_i2s_rms());
    fb_text_mw(x0, L_MAIN_Y0 + 28, line, t->dim, cw);

    static const char *const opts[3] = {"播放", "暂停", "停止"};
    for (int i = 0; i < 3; ++i) {
        int y = L_MAIN_Y0 + 64 + i * 28;
        if (i == sel && ph >= MIC_LAB_READY && ph <= MIC_LAB_PAUSE) {
            fb_round_rect(x0, y - 4, cw, 26, 6, t->accent);
            fb_text_mw(x0 + 8, y, opts[i], t->on_accent, cw - 16);
        } else {
            fb_text_mw(x0 + 10, y, opts[i], t->dim, cw - 16);
        }
    }
    fb_text_mw(x0, L_PARAM_Y0, "长按退出  不存文件", t->dim, cw);
    flush_fb();
}

void ui_tft_miclab_enter(void)
{
    if (s_page == UI_PAGE_PAINT) {
        s_paint_lx = s_paint_ly = -1;
    }
    sd_music_cmd("stop");
    s_page = UI_PAGE_MICLAB;
    draw_miclab_frame();
}

void ui_tft_miclab_exit(void)
{
    if (s_page != UI_PAGE_MICLAB) {
        return;
    }
    resume_ui_home();
}

int ui_tft_miclab_active(void)
{
    return s_page == UI_PAGE_MICLAB;
}

static void on_miclab_ui(int enter)
{
    if (enter) {
        ui_tft_miclab_enter();
    } else {
        ui_tft_miclab_exit();
    }
}

static void paint_corners(void)
{
    const ui_theme_t *t = ui_theme();
    const int m = TFT_SAFE;
    const int s = 14;
    fb_rect(m, m, s, s, t->accent);
    fb_rect(s_w - m - s, m, s, s, C_GREEN);
    fb_rect(m, s_h - m - s, s, s, C_BLUE);
    fb_rect(s_w - m - s, s_h - m - s, s, s, C_RED);
}

static void splash(uint16_t color, const char *msg)
{
    fb_fill(color);
    fb_text(TFT_SAFE + 4, s_h / 2 - 20, msg, C_WHITE);
    fb_text(TFT_SAFE + 4, s_h / 2 + 4, "ST7789P3 284x240", C_WHITE);
    paint_corners();
    flush_fb();
}

static void draw_bringup(void)
{
    const ui_theme_t *t = ui_theme();
    const int m = TFT_SAFE;
    fb_fill_theme_bg();
    fb_round_rect(m, m, s_w - 2 * m, 28, 6, t->panel);
    fb_text(m + 8, m + 6, "SKY-music", t->accent);
    fb_text(m + 8, m + 40, "ST7789P3", t->text);
    char line[40];
    snprintf(line, sizeof(line), "%dx%d rot=%d theme=%s", s_w, s_h, s_rot, ui_theme_name());
    fb_text(m + 8, m + 64, line, t->dim);
    snprintf(line, sizeof(line), "gap %d,%d safe=%d", s_col_off, s_row_off, m);
    fb_text(m + 8, m + 88, line, t->dim);
    fb_text(m + 8, m + 120, "tft theme sky|night", t->dim);
    fb_text(m + 8, m + 144, "tft gap / tft inv", t->dim);
    paint_corners();
    flush_fb();
}

static void pause_ui_for_debug(void)
{
    s_page = UI_PAGE_DEBUG;
}

static void resume_ui_home(void)
{
    /* State only — never flush here. ENC hooks run on key_link task; concurrent
     * flush with ui_task deadlocks SPI wait and freezes the whole HUD. */
    s_pending_mode_click = 0;
    s_page = UI_PAGE_HOME;
    const char *mode = enc_fn_mode_name();
    if (mode) {
        strncpy(s_last_mode, mode, sizeof(s_last_mode) - 1);
        s_last_mode[sizeof(s_last_mode) - 1] = 0;
    }
    s_home_list_idx = enc_fn_list_idx();
    s_nav_lock_until = xTaskGetTickCount() + pdMS_TO_TICKS(400);
}

static void flush_pending_mode_click(void)
{
    if (!s_pending_mode_click) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if ((now - s_pending_mode_click_at) < pdMS_TO_TICKS(DBL_MS)) {
        return;
    }
    s_pending_mode_click = 0;
    s_enc1_last_click = 0; /* action click is not half of a double */
    if (s_page != UI_PAGE_MODE) {
        return;
    }
    s_flushing_deferred = 1;
    enc_fn_click();
    s_flushing_deferred = 0;
}

static void flush_pending_home_click(void)
{
    if (!s_pending_home_click) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if ((now - s_pending_home_click_at) < pdMS_TO_TICKS(DBL_MS)) {
        return;
    }
    s_pending_home_click = 0;
    s_enc1_last_click = 0;
    if (s_page != UI_PAGE_HOME || s_qp) {
        return;
    }
    s_page = UI_PAGE_LIBRARY;
    s_lib_sel = 0;
}

static void ui_task(void *arg)
{
    (void)arg;
    s_fps_tick = xTaskGetTickCount();
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        flush_pending_home_click();
        flush_pending_mode_click();
        if (s_page == UI_PAGE_SPLASH) {
            if ((int32_t)(now - s_splash_until) >= 0) {
                resume_ui_home();
            }
        } else if (s_page == UI_PAGE_HOME || s_page == UI_PAGE_LIBRARY || s_page == UI_PAGE_MODE) {
            ui_sync_page_from_mode();
            draw_ui_frame();
            s_lcd_frames++;
            TickType_t dt = now - s_fps_tick;
            if (dt >= pdMS_TO_TICKS(500)) {
                uint32_t ms = (uint32_t)dt * portTICK_PERIOD_MS;
                if (ms == 0) {
                    ms = 1;
                }
                s_lcd_fps = (s_lcd_frames * 1000u) / ms;
                s_lcd_frames = 0;
                s_fps_tick = now;
            }
        } else if (s_page == UI_PAGE_MICLAB) {
            draw_miclab_frame();
        } else if (s_page == UI_PAGE_PAINT && s_paint_hud_dirty) {
            /* stroke path flushes ink; only refresh HUD if stale */
            if ((now - s_paint_hud_at) >= pdMS_TO_TICKS(120)) {
                paint_draw_hud();
                flush_fb();
                s_paint_hud_at = now;
                s_paint_hud_dirty = 0;
            }
        }
        int delay = s_frame_ms;
        /* Audio / decode contended with SPI+PSRAM — slow UI to reduce flicker. */
        int mplay = 0, splay = 0, spause = 0;
        uint32_t _ms = 0, _dur = 0;
        unsigned _pct = 0;
        sd_music_get_progress(&_ms, &_dur, &_pct, &mplay);
        score_get_progress(&_ms, &_dur, &_pct, &splay, &spause);
        if ((mixer_active_voices() > 0 || mplay || splay) && delay < 110) {
            delay = 110;
        }
        /* While waiting to distinguish click vs double-click, poll sooner */
        if (s_pending_mode_click && delay > 40) {
            delay = 40;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

bool ui_tft_handle_line(const char *line)
{
    if (!line || strncmp(line, "tft", 3) != 0) {
        return false;
    }
    if (line[3] == 0) {
        printf("tft ST7789P3 rot=%d madctl=0x%02x %dx%d off=%d,%d flip=%d blk=%d inv=%d page=%d\n",
               s_rot, s_madctl, s_w, s_h, s_col_off, s_row_off, s_flip180, s_blk, (int)s_invert,
               (int)s_page);
        printf("cmds: tft home | tft theme [sky|night|+] | tft bg <path>|clear | tft qp | tft lib\n");
        return true;
    }
    unsigned a = 0;
    int c = 0, r = 0;
    if (strncmp(line, "tft theme", 9) == 0) {
        const char *arg = line + 9;
        while (*arg == ' ') {
            arg++;
        }
        if (ui_theme_set_name(*arg ? arg : "+") != 0) {
            printf("theme: sky | night | +\n");
            return true;
        }
        (void)ui_bg_load_for_theme();
        printf("theme=%s bg=%s\n", ui_theme_name(), ui_bg_path());
        if (s_page == UI_PAGE_HOME || s_page == UI_PAGE_LIBRARY || s_page == UI_PAGE_MODE) {
            draw_ui_frame();
        } else if (s_page == UI_PAGE_DEBUG) {
            draw_bringup();
        }
        return true;
    }
    if (strncmp(line, "tft bg", 6) == 0) {
        const char *arg = line + 6;
        while (*arg == ' ') {
            arg++;
        }
        if (!arg[0] || strcmp(arg, "?") == 0) {
            printf("bg ready=%d path=%s\n", ui_bg_ready(), ui_bg_path());
            printf("cmds: tft bg /sdcard/ui/foo.raw | tft bg clear | tft bg reload\n");
            return true;
        }
        if (strcmp(arg, "clear") == 0) {
            ui_bg_clear_override();
            (void)ui_bg_load_for_theme();
        } else if (strcmp(arg, "reload") == 0) {
            (void)ui_bg_load_for_theme();
        } else if (ui_bg_set_override(arg) != 0) {
            printf("bg load fail: %s\n", arg);
            return true;
        }
        printf("bg=%s\n", ui_bg_path());
        if (s_page == UI_PAGE_HOME || s_page == UI_PAGE_LIBRARY || s_page == UI_PAGE_MODE) {
            draw_ui_frame();
        }
        return true;
    }
    if (strcmp(line, "tft home") == 0) {
        resume_ui_home();
        return true;
    }
    if (strcmp(line, "tft lib") == 0) {
        s_page = UI_PAGE_LIBRARY;
        draw_ui_frame();
        return true;
    }
    if (strcmp(line, "tft qp") == 0) {
        ui_tft_quick_panel_toggle();
        if (s_page == UI_PAGE_HOME || s_page == UI_PAGE_MODE) {
            draw_ui_frame();
        }
        return true;
    }
    if (strcmp(line, "tft next") == 0) {
        pause_ui_for_debug();
        set_rotation(s_rot + 1);
        draw_bringup();
        return true;
    }
    if (strcmp(line, "tft test") == 0) {
        pause_ui_for_debug();
        splash(C_RED, "RED");
        return true;
    }
    if (strcmp(line, "tft hud") == 0) {
        if (s_page == UI_PAGE_DEBUG) {
            resume_ui_home();
        } else {
            pause_ui_for_debug();
            draw_bringup();
        }
        return true;
    }
    if (sscanf(line, "tft rot %u", &a) == 1) {
        pause_ui_for_debug();
        set_rotation((int)a);
        s_flip180 = 0;
        draw_bringup();
        return true;
    }
    if (sscanf(line, "tft flip %u", &a) == 1) {
        pause_ui_for_debug();
        s_flip180 = a ? 1 : 0;
        draw_bringup();
        return true;
    }
    if (sscanf(line, "tft gap %d %d", &c, &r) == 2) {
        pause_ui_for_debug();
        s_col_off = c;
        s_row_off = r;
        draw_bringup();
        return true;
    }
    if (sscanf(line, "tft blk %u", &a) == 1) {
        s_blk = a ? 1 : 0;
        gpio_set_level(PIN_TFT_BLK, s_blk);
        return true;
    }
    if (sscanf(line, "tft inv %u", &a) == 1) {
        pause_ui_for_debug();
        s_invert = a != 0;
        lcd_cmd(s_invert ? 0x21 : 0x20, NULL, 0);
        draw_bringup();
        return true;
    }
    return true;
}

esp_err_t ui_tft_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_RST) | (1ULL << PIN_TFT_BLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_TFT_BLK, 0);
    gpio_set_level(PIN_TFT_RST, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_TFT_SCK,
        .mosi_io_num = PIN_TFT_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_PHYS_H * STRIP_H * 2 + 64,
    };
    esp_err_t err = spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus: %s", esp_err_to_name(err));
        return err;
    }

    s_xfer_done = xSemaphoreCreateBinary();
    s_draw_mu = xSemaphoreCreateMutex();
    if (!s_xfer_done || !s_draw_mu) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_spi_config_t iocfg = {
        .cs_gpio_num = PIN_TFT_CS,
        .dc_gpio_num = PIN_TFT_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_SPI_HOST, &iocfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io: %s", esp_err_to_name(err));
        return err;
    }

    s_fb = heap_caps_aligned_alloc(64, FB_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "no PSRAM for fb");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, FB_MAX * sizeof(uint16_t));
    (void)ui_bg_init(); /* PSRAM slot; file load after factory mount */

    size_t strip_bytes = (size_t)TFT_PHYS_H * STRIP_H * sizeof(uint16_t);
    s_strip[0] = heap_caps_aligned_alloc(32, strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_strip[1] = heap_caps_aligned_alloc(32, strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip[0] || !s_strip[1]) {
        ESP_LOGE(TAG, "no DMA strip pair");
        return ESP_ERR_NO_MEM;
    }
    s_strip_idx = 0;

    st7789_init();
    set_rotation(1); /* V2 touch panel landscape upright (was rot=3 on non-touch) */
    s_flip180 = 0;
    /* Light panel ASAP so boot never looks "dead". */
    gpio_set_level(PIN_TFT_BLK, 1);
    s_blk = 1;
    s_page = UI_PAGE_SPLASH;
    s_splash_until = xTaskGetTickCount() + pdMS_TO_TICKS(SPLASH_MS);
    draw_brand_splash();
    enc_fn_set_nav_hooks(ui_tft_on_enc1_rotate, ui_tft_on_enc1_click, ui_tft_on_enc1_long);
    mic_lab_set_ui_cb(on_miclab_ui);

    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "tft", 4096, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ST7789P3 %dx%d splash→home SCL=%d SDA=%d CS=%d DC=%d RST=%d BLK=%d", s_w, s_h,
             (int)PIN_TFT_SCK, (int)PIN_TFT_MOSI, (int)PIN_TFT_CS, (int)PIN_TFT_DC,
             (int)PIN_TFT_RST, (int)PIN_TFT_BLK);
    return ESP_OK;
}

void ui_tft_set_net(const char *line)
{
    if (!line) {
        s_net[0] = 0;
        return;
    }
    strncpy(s_net, line, sizeof(s_net) - 1);
    s_net[sizeof(s_net) - 1] = 0;
}

void ui_tft_radio_busy(bool busy)
{
    if (busy) {
        s_radio_busy++;
    } else if (s_radio_busy > 0) {
        s_radio_busy--;
    }
    s_frame_ms = s_radio_busy ? 120 : 50;
}
