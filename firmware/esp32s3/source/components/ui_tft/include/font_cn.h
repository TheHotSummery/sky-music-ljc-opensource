#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FONT_CN_H 14
typedef struct {
    uint16_t cp;
    uint8_t w;
    uint8_t nbytes;
    uint32_t off;
} font_cn_glyph_t;
extern const font_cn_glyph_t font_cn_glyphs[];
extern const uint8_t font_cn_bits[];
extern const int font_cn_glyph_n;
const font_cn_glyph_t *font_cn_find(uint32_t cp);
int font_cn_width(const char *utf8);
int font_cn_draw(uint16_t *fb, int stride, int fb_h, int x, int y, const char *utf8,
                 uint16_t color, int max_w);
/* Draw with horizontal pixel clip [clip_x0, clip_x0+clip_w). Returns drawn advance. */
int font_cn_draw_clip(uint16_t *fb, int stride, int fb_h, int x, int y, const char *utf8,
                      uint16_t color, int clip_x0, int clip_w);
#ifdef __cplusplus
}
#endif
