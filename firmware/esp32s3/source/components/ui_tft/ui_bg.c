#include "ui_bg.h"
#include "ui_theme.h"

#include "board_pins.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_bg";

static uint16_t *s_bg;
static int s_ready;
static char s_path[96];
static char s_override[96];

esp_err_t ui_bg_init(void)
{
    if (s_bg) {
        return ESP_OK;
    }
    s_bg = heap_caps_aligned_alloc(64, UI_BG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg) {
        ESP_LOGE(TAG, "no PSRAM for bg");
        return ESP_ERR_NO_MEM;
    }
    memset(s_bg, 0, UI_BG_BYTES);
    s_ready = 0;
    s_path[0] = 0;
    s_override[0] = 0;
    return ESP_OK;
}

int ui_bg_ready(void)
{
    return s_ready && s_bg;
}

const char *ui_bg_path(void)
{
    return s_path[0] ? s_path : "-";
}

static int load_raw(const char *path)
{
    if (!s_bg || !path || !path[0]) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t n = fread(s_bg, 1, UI_BG_BYTES, f);
    fclose(f);
    if (n != UI_BG_BYTES) {
        ESP_LOGW(TAG, "short read %s (%u)", path, (unsigned)n);
        s_ready = 0;
        return -1;
    }
    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = 0;
    s_ready = 1;
    ESP_LOGI(TAG, "loaded %s", path);
    return 0;
}

int ui_bg_load_file(const char *path)
{
    return load_raw(path);
}

void ui_bg_clear_override(void)
{
    s_override[0] = 0;
}

int ui_bg_set_override(const char *path)
{
    if (!path || !path[0]) {
        ui_bg_clear_override();
        return ui_bg_load_for_theme();
    }
    strncpy(s_override, path, sizeof(s_override) - 1);
    s_override[sizeof(s_override) - 1] = 0;
    return load_raw(s_override);
}

int ui_bg_load_for_theme(void)
{
    if (!s_bg && ui_bg_init() != ESP_OK) {
        return -1;
    }
    if (s_override[0]) {
        if (load_raw(s_override) == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "override missing, fall back");
    }

    const char *theme = ui_theme_name();
    char path[96];

    snprintf(path, sizeof(path), "%s/ui/bg_%s.raw", SD_MOUNT_POINT, theme);
    if (load_raw(path) == 0) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s/ui/bg_%s.raw", FACTORY_MOUNT_POINT, theme);
    if (load_raw(path) == 0) {
        return 0;
    }
    /* ultimate default */
    snprintf(path, sizeof(path), "%s/ui/bg_sky.raw", FACTORY_MOUNT_POINT);
    if (load_raw(path) == 0) {
        return 0;
    }
    s_ready = 0;
    return -1;
}

void ui_bg_blit(uint16_t *fb, int w, int h)
{
    if (!ui_bg_ready() || !fb) {
        return;
    }
    if (w == UI_BG_W && h == UI_BG_H) {
        memcpy(fb, s_bg, UI_BG_BYTES);
        return;
    }
    /* center-crop / clip if size mismatch */
    int copy_w = w < UI_BG_W ? w : UI_BG_W;
    int copy_h = h < UI_BG_H ? h : UI_BG_H;
    for (int y = 0; y < copy_h; ++y) {
        memcpy(&fb[y * w], &s_bg[y * UI_BG_W], (size_t)copy_w * 2);
    }
}
