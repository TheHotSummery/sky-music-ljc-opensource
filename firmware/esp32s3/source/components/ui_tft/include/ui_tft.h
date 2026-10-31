#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_tft_start(void);
/* Call after enc_fn_start(): unlock ENC1 nav (avoids boot mode-scan yanking Home). */
void ui_tft_nav_enable(void);
/* After factory/SD mount: load theme background into PSRAM (one slot). */
void ui_tft_bg_reload(void);
bool ui_tft_handle_line(const char *line);
/* WiFi/BLE 占用 GDMA 时放慢刷屏，避免 ST7735 丢字节出现右移/发灰 */
void ui_tft_radio_busy(bool busy);
void ui_tft_set_net(const char *line);
/* ENC2 long-press (≥700ms, no rotate): Quick Panel vol/key/tempo */
void ui_tft_quick_panel_toggle(void);
/* ENC1 nav intercept for Home/Library. Return true if consumed. */
bool ui_tft_on_enc1_rotate(int delta);
bool ui_tft_on_enc1_click(void);
bool ui_tft_on_enc1_long(void);

/* Bring-up: touch paint board (absolute XY, no ENC emulation). */
int ui_tft_paint_active(void);
void ui_tft_paint_enter(void);
void ui_tft_paint_exit(void);
void ui_tft_paint_clear(void);
/* down=1 while finger contacts; stroke connects points. flush=1 after burst. */
void ui_tft_paint_stroke(int x, int y, int down, int flush);
void ui_tft_paint_set_hud(int raw_x, int raw_y, int ui_x, int ui_y);

/* Bring-up: mic lab page (driven by mic_i2s lab state). */
void ui_tft_miclab_enter(void);
void ui_tft_miclab_exit(void);
int ui_tft_miclab_active(void);

#ifdef __cplusplus
}
#endif
