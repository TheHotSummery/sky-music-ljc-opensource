#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_THEME_SKY = 0,
    UI_THEME_NIGHT,
    UI_THEME_COUNT,
} ui_theme_id_t;

/* RGB565 roles — pages must not hardcode accents. */
typedef struct {
    const char *name;
    uint16_t bg_top;
    uint16_t bg_bot;
    uint16_t panel;
    uint16_t panel2;
    uint16_t text;
    uint16_t dim;
    uint16_t accent;
    uint16_t on_accent; /* text/icon on accent pills */
    uint16_t ok;
    uint16_t warn;
    uint16_t line;
    uint16_t gutter;
    uint16_t sep;
    uint16_t note_cur;
    uint16_t note_next;
    uint16_t bad;
    uint16_t white;
} ui_theme_t;

const ui_theme_t *ui_theme(void);
ui_theme_id_t ui_theme_id(void);
const char *ui_theme_name(void);
void ui_theme_set(ui_theme_id_t id);
/* Cycle sky → night → … ; returns new name. */
const char *ui_theme_next(void);
/* "sky" / "night" / "+" / NULL(=next). Returns 0 ok. */
int ui_theme_set_name(const char *name);

#ifdef __cplusplus
}
#endif
