#include "font_cn.h"

#include <stddef.h>

static int utf8_next(const char **ps, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)*ps;
    if (!s || !*s) {
        return 0;
    }
    unsigned char c0 = s[0];
    if (c0 < 0x80) {
        *cp = c0;
        *ps = (const char *)(s + 1);
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0 && s[1]) {
        *cp = ((uint32_t)(c0 & 0x1F) << 6) | (s[1] & 0x3F);
        *ps = (const char *)(s + 2);
        return 1;
    }
    if ((c0 & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *ps = (const char *)(s + 3);
        return 1;
    }
    if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *cp = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *ps = (const char *)(s + 4);
        return 1;
    }
    *cp = '?';
    *ps = (const char *)(s + 1);
    return 1;
}

const font_cn_glyph_t *font_cn_find(uint32_t cp)
{
    if (cp > 0xFFFF) {
        return NULL;
    }
    int lo = 0, hi = font_cn_glyph_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint16_t v = font_cn_glyphs[mid].cp;
        if (v == (uint16_t)cp) {
            return &font_cn_glyphs[mid];
        }
        if (v < (uint16_t)cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static void blit_glyph_clip(uint16_t *fb, int stride, int fb_h, int x, int y, const font_cn_glyph_t *g,
                            uint16_t color, int clip_x0, int clip_x1)
{
    if (!fb || !g) {
        return;
    }
    const uint8_t *bits = font_cn_bits + g->off;
    int row_bytes = (g->w + 7) >> 3;
    for (int row = 0; row < FONT_CN_H; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= fb_h) {
            continue;
        }
        const uint8_t *src = bits + row * row_bytes;
        for (int col = 0; col < g->w; ++col) {
            int xx = x + col;
            if (xx < clip_x0 || xx >= clip_x1 || xx < 0 || xx >= stride) {
                continue;
            }
            if (src[col >> 3] & (uint8_t)(0x80 >> (col & 7))) {
                fb[yy * stride + xx] = color;
            }
        }
    }
}

static void blit_glyph(uint16_t *fb, int stride, int fb_h, int x, int y, const font_cn_glyph_t *g,
                       uint16_t color)
{
    blit_glyph_clip(fb, stride, fb_h, x, y, g, color, 0, stride);
}

int font_cn_width(const char *utf8)
{
    if (!utf8) {
        return 0;
    }
    int w = 0;
    const char *p = utf8;
    uint32_t cp;
    while (utf8_next(&p, &cp)) {
        const font_cn_glyph_t *g = font_cn_find(cp);
        w += g ? g->w + 1 : (FONT_CN_H / 2);
    }
    return w;
}

int font_cn_draw(uint16_t *fb, int stride, int fb_h, int x, int y, const char *utf8, uint16_t color,
                 int max_w)
{
    if (!fb || !utf8 || fb_h <= 0 || stride <= 0) {
        return 0;
    }
    int x0 = x;
    const char *p = utf8;
    uint32_t cp;
    while (utf8_next(&p, &cp)) {
        const font_cn_glyph_t *g = font_cn_find(cp);
        int adv = g ? g->w + 1 : (FONT_CN_H / 2);
        if (max_w > 0 && (x - x0) + adv > max_w) {
            break;
        }
        if (g) {
            blit_glyph(fb, stride, fb_h, x, y, g, color);
        } else {
            for (int yy = 0; yy < FONT_CN_H - 2; ++yy) {
                int py = y + 1 + yy;
                if (py < 0 || py >= fb_h) {
                    continue;
                }
                for (int xx = 0; xx < adv - 1; ++xx) {
                    int px = x + xx;
                    if (px < 0 || px >= stride) {
                        continue;
                    }
                    if (yy == 0 || yy == FONT_CN_H - 3 || xx == 0 || xx == adv - 2) {
                        fb[py * stride + px] = color;
                    }
                }
            }
        }
        x += adv;
    }
    return x - x0;
}

int font_cn_draw_clip(uint16_t *fb, int stride, int fb_h, int x, int y, const char *utf8,
                      uint16_t color, int clip_x0, int clip_w)
{
    if (!fb || !utf8 || fb_h <= 0 || stride <= 0 || clip_w < 1) {
        return 0;
    }
    int clip_x1 = clip_x0 + clip_w;
    int x0 = x;
    const char *p = utf8;
    uint32_t cp;
    while (utf8_next(&p, &cp)) {
        const font_cn_glyph_t *g = font_cn_find(cp);
        int adv = g ? g->w + 1 : (FONT_CN_H / 2);
        if (x + adv < clip_x0) {
            x += adv;
            continue;
        }
        if (x >= clip_x1) {
            break;
        }
        if (g) {
            blit_glyph_clip(fb, stride, fb_h, x, y, g, color, clip_x0, clip_x1);
        }
        x += adv;
    }
    return x - x0;
}
