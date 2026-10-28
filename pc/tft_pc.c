#include "tft.h"
#include "gfx.h"
#include <string.h>
uint16_t pc_screen[GFX_W * GFX_H];
void tft_write_rows(int y0, int y1, const uint16_t *fb)
{
    memcpy(&pc_screen[y0 * GFX_W], fb + y0 * GFX_W, (size_t)(y1 - y0 + 1) * GFX_W * 2);
}
