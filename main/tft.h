#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef GFX_PC
#include "esp_err.h"
// Adds the ST7735 to the shared SPI2 bus (board_spi_init() must have run) and initialises the panel.
esp_err_t tft_init(void);
// Backlight on GPIO PIN_TFT_BL. Left off by tft_init() so the first frame is not preceded by a white flash.
void tft_backlight(bool on);
#endif
// Sends full-width rows y0..y1 (inclusive) from a GFX_W x GFX_H framebuffer.
// Pixels are RGB565, already byte-swapped for the panel.
void tft_write_rows(int y0, int y1, const uint16_t *fb);
