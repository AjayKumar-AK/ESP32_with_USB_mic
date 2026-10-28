#pragma once
#include <stdint.h>

typedef struct {
    uint32_t off;      // offset into the alpha bitmap
    uint8_t  w, h;     // glyph bitmap size
    int8_t   xo, yo;   // bitmap origin relative to pen x / top of line
    uint8_t  adv;      // horizontal advance
} glyph_t;

typedef struct {
    const glyph_t  *glyphs;
    const uint8_t  *bmp;     // 8-bit alpha
    uint8_t first, last;
    uint8_t height;          // line height
    uint8_t ascent;
} font_t;

extern const font_t font_sm;    // 10 px regular
extern const font_t font_smb;   // 10 px bold
extern const font_t font_md;    // 13 px bold
extern const font_t font_lg;    // 20 px bold
extern const font_t font_xl;    // 52 px bold, digits and ':' only
