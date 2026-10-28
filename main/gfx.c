#include "gfx.h"
#include "tft.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef GFX_PC
#include "esp_heap_caps.h"
#endif

// The framebuffer stores pixels byte-swapped, ready to go straight to the panel.
static uint16_t *s_fb;
static int s_dy0 = GFX_H, s_dy1 = -1;

static inline uint16_t sw(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline void touch(int y0, int y1)
{
    if (y0 < s_dy0) s_dy0 = y0;
    if (y1 > s_dy1) s_dy1 = y1;
}

bool gfx_init(void)
{
#ifdef GFX_PC
    s_fb = calloc((size_t)GFX_W * GFX_H, 2);
#else
    s_fb = heap_caps_calloc((size_t)GFX_W * GFX_H, 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif
    return s_fb != NULL;
}

void gfx_flush(void)
{
    if (s_dy1 < s_dy0) return;
    tft_write_rows(s_dy0, s_dy1, s_fb);
    s_dy0 = GFX_H;
    s_dy1 = -1;
}

void gfx_fill_rect(int x, int y, int w, int h, color_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > GFX_W) x1 = GFX_W;
    if (y1 > GFX_H) y1 = GFX_H;
    if (x >= x1 || y >= y1) return;
    const uint16_t v = sw(c);
    for (int j = y; j < y1; j++) {
        uint16_t *row = s_fb + (size_t)j * GFX_W;
        for (int i = x; i < x1; i++) row[i] = v;
    }
    touch(y, y1 - 1);
}

void gfx_fill(color_t c) { gfx_fill_rect(0, 0, GFX_W, GFX_H, c); }

void gfx_fill_round_rect(int x, int y, int w, int h, int r, color_t c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int k = j < r ? j : (j >= h - r ? h - 1 - j : r);
        int inset = 0;
        if (k < r) {
            float dy = (float)r - (float)k - 0.5f;
            inset = (int)ceilf((float)r - sqrtf((float)(r * r) - dy * dy));
        }
        gfx_fill_rect(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

void gfx_fill_round_rect_border(int x, int y, int w, int h, int r, color_t border, color_t fill)
{
    gfx_fill_round_rect(x, y, w, h, r, border);
    gfx_fill_round_rect(x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, fill);
}

void gfx_fill_circle(int cx, int cy, int r, color_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        gfx_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

// ----------------------------------------------------------------- text
static inline color_t blend(color_t bg, color_t fg, uint8_t a)
{
    int br = (bg >> 11) & 31, bgc = (bg >> 5) & 63, bb = bg & 31;
    int fr = (fg >> 11) & 31, fgc = (fg >> 5) & 63, fb = fg & 31;
    int r = (br * (255 - a) + fr * a + 127) / 255;
    int g = (bgc * (255 - a) + fgc * a + 127) / 255;
    int b = (bb * (255 - a) + fb * a + 127) / 255;
    return (color_t)((r << 11) | (g << 5) | b);
}

static const glyph_t *glyph_for(const font_t *f, char ch)
{
    unsigned c = (unsigned char)ch;
    if (c < f->first || c > f->last) c = '?';
    if (c < f->first || c > f->last) return NULL;
    return &f->glyphs[c - f->first];
}

int gfx_text_width(const font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        const glyph_t *g = glyph_for(f, *s);
        if (g) w += g->adv;
    }
    return w;
}

int gfx_text(const font_t *f, int x, int y, const char *s, color_t fg)
{
    int pen = x;
    for (; *s; s++) {
        const glyph_t *g = glyph_for(f, *s);
        if (!g) continue;
        const uint8_t *bm = f->bmp + g->off;
        for (int j = 0; j < g->h; j++) {
            int yy = y + g->yo + j;
            if (yy < 0 || yy >= GFX_H) continue;
            for (int i = 0; i < g->w; i++) {
                int xx = pen + g->xo + i;
                if (xx < 0 || xx >= GFX_W) continue;
                uint8_t a = bm[j * g->w + i];
                if (a < 8) continue;
                uint16_t *p = &s_fb[(size_t)yy * GFX_W + xx];
                *p = a > 247 ? sw(fg) : sw(blend(sw(*p), fg, a));
            }
        }
        touch(y + g->yo > 0 ? y + g->yo : 0, y + g->yo + g->h - 1 < GFX_H ? y + g->yo + g->h - 1 : GFX_H - 1);
        pen += g->adv;
    }
    return pen - x;
}

void gfx_text_center(const font_t *f, int cx, int cy, const char *s, color_t fg)
{
    gfx_text(f, cx - gfx_text_width(f, s) / 2, cy - f->height / 2, s, fg);
}

void gfx_text_fit(const font_t *const *fonts, int n, int cx, int cy, int max_w,
                  const char *s, color_t fg)
{
    for (int i = 0; i < n; i++) {
        if (gfx_text_width(fonts[i], s) <= max_w) {
            gfx_text_center(fonts[i], cx, cy, s, fg);
            return;
        }
    }
    const font_t *f = fonts[n - 1];
    char buf[64];
    size_t len = strlen(s);
    if (len > sizeof(buf) - 4) len = sizeof(buf) - 4;
    memcpy(buf, s, len);
    buf[len] = 0;
    while (len > 1) {
        memcpy(buf + len, "...", 4);
        if (gfx_text_width(f, buf) <= max_w) break;
        buf[--len] = 0;
    }
    gfx_text_center(f, cx, cy, buf, fg);
}
