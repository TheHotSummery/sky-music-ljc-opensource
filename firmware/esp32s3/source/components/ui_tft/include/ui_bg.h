#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_BG_W 284
#define UI_BG_H 240
#define UI_BG_BYTES (UI_BG_W * UI_BG_H * 2)

esp_err_t ui_bg_init(void);
/* 1 if a full-frame RGB565 image is resident in PSRAM. */
int ui_bg_ready(void);
const char *ui_bg_path(void);

/*
 * Load RGB565 little-endian raw (284x240) into the single PSRAM slot.
 * Path examples:
 *   /factory/ui/bg_sky.raw
 *   /sdcard/ui/bg_custom.raw
 * Returns 0 on success.
 */
int ui_bg_load_file(const char *path);

/* Clear host override; next reload uses theme defaults. */
void ui_bg_clear_override(void);

/*
 * Resolve background for current theme:
 *   1) host override path (if set)
 *   2) /sdcard/ui/bg_<theme>.raw
 *   3) /factory/ui/bg_<theme>.raw
 *   4) /factory/ui/bg_sky.raw
 * Returns 0 on success.
 */
int ui_bg_load_for_theme(void);

/* Copy resident bg into framebuffer (no-op if not ready). */
void ui_bg_blit(uint16_t *fb, int w, int h);

/* Host/上位机: set override path and load (NULL clears). */
int ui_bg_set_override(const char *path);

#ifdef __cplusplus
}
#endif
