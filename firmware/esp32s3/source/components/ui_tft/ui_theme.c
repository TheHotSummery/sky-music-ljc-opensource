#include "ui_theme.h"

#include <strings.h>
#include <string.h>

#define RGB565(r, g, b)                                                                                                \
    (uint16_t)(((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Concept: 晴空 — light airy sky, warm gold focus (Sky: CotL vibe, MCU-safe). */
static const ui_theme_t s_sky = {
    .name = "sky",
    .bg_top = RGB565(148, 190, 228),
    .bg_bot = RGB565(232, 242, 250),
    .panel = RGB565(248, 252, 255),
    .panel2 = RGB565(220, 236, 248),
    .text = RGB565(36, 48, 68),
    .dim = RGB565(90, 118, 148),
    .accent = RGB565(240, 186, 72),
    .on_accent = RGB565(40, 44, 56),
    .ok = RGB565(56, 180, 120),
    .warn = RGB565(255, 150, 60),
    .line = RGB565(170, 200, 225),
    .gutter = RGB565(190, 215, 235),
    .sep = RGB565(110, 150, 190),
    .note_cur = RGB565(72, 170, 130),
    .note_next = RGB565(255, 220, 120),
    .bad = RGB565(220, 70, 80),
    .white = RGB565(255, 255, 255),
};

/* Concept: 夜空 — deep navy, soft lilac accent. */
static const ui_theme_t s_night = {
    .name = "night",
    .bg_top = RGB565(10, 14, 32),
    .bg_bot = RGB565(28, 34, 64),
    .panel = RGB565(36, 44, 78),
    .panel2 = RGB565(28, 40, 68),
    .text = RGB565(232, 236, 248),
    .dim = RGB565(130, 145, 175),
    .accent = RGB565(196, 168, 255),
    .on_accent = RGB565(24, 20, 48),
    .ok = RGB565(70, 210, 150),
    .warn = RGB565(255, 170, 70),
    .line = RGB565(55, 70, 110),
    .gutter = RGB565(40, 50, 80),
    .sep = RGB565(100, 130, 190),
    .note_cur = RGB565(80, 200, 150),
    .note_next = RGB565(210, 190, 255),
    .bad = RGB565(240, 90, 100),
    .white = RGB565(255, 255, 255),
};

static const ui_theme_t *const s_table[UI_THEME_COUNT] = {&s_sky, &s_night};
static ui_theme_id_t s_id = UI_THEME_SKY;

const ui_theme_t *ui_theme(void)
{
    return s_table[s_id];
}

ui_theme_id_t ui_theme_id(void)
{
    return s_id;
}

const char *ui_theme_name(void)
{
    return s_table[s_id]->name;
}

void ui_theme_set(ui_theme_id_t id)
{
    if ((unsigned)id >= (unsigned)UI_THEME_COUNT) {
        id = UI_THEME_SKY;
    }
    s_id = id;
}

const char *ui_theme_next(void)
{
    s_id = (ui_theme_id_t)(((int)s_id + 1) % (int)UI_THEME_COUNT);
    return s_table[s_id]->name;
}

int ui_theme_set_name(const char *name)
{
    if (!name || !name[0] || strcmp(name, "+") == 0 || strcmp(name, "next") == 0) {
        (void)ui_theme_next();
        return 0;
    }
    for (int i = 0; i < (int)UI_THEME_COUNT; ++i) {
        if (strcasecmp(s_table[i]->name, name) == 0) {
            s_id = (ui_theme_id_t)i;
            return 0;
        }
    }
    return -1;
}
