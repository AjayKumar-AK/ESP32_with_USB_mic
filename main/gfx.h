#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "font.h"

#define GFX_W 160
#define GFX_H 128

typedef uint16_t color_t;   // RGB565, normal byte order
#define GFX_RGB(r, g, b) ((color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

bool gfx_init(void);        // allocates the framebuffer (the panel must be initialised separately)
void gfx_flush(void);       // pushes only the rows changed since the last flush

void gfx_fill(color_t c);
void gfx_fill_rect(int x, int y, int w, int h, color_t c);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, color_t c);
void gfx_fill_round_rect_border(int x, int y, int w, int h, int r, color_t border, color_t fill);
void gfx_fill_circle(int cx, int cy, int r, color_t c);

int  gfx_text_width(const font_t *f, const char *s);
int  gfx_text(const font_t *f, int x, int y, const char *s, color_t fg);            // y = top of line
void gfx_text_center(const font_t *f, int cx, int cy, const char *s, color_t fg);   // cy = vertical centre
// Uses the first font in the list whose width fits max_w; otherwise ellipsizes with the last one.
void gfx_text_fit(const font_t *const *fonts, int n, int cx, int cy, int max_w,
                  const char *s, color_t fg);
