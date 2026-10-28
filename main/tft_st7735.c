#include "tft.h"
#include "gfx.h"
#include "app_config.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tft";
static spi_device_handle_t s_spi;

_Static_assert(GFX_W * 2 <= SPI_BUS_MAX_XFER_BYTES, "one row must fit in one SPI transfer");

static void dc_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_TFT_DC, (int)(intptr_t)t->user);
}

static void tx(const void *buf, size_t len, int is_data)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
        .user = (void *)(intptr_t)is_data,
    };
    esp_err_t e = spi_device_transmit(s_spi, &t);
    if (e != ESP_OK) ESP_LOGE(TAG, "spi transmit failed: %s", esp_err_to_name(e));
}

static void cmd(uint8_t c) { tx(&c, 1, 0); }
static void cmd_data(uint8_t c, const uint8_t *d, size_t n) { cmd(c); if (n) tx(d, n, 1); }

typedef struct { uint8_t cmd; uint8_t n; uint8_t d[16]; uint16_t delay_ms; } init_step_t;

#define MADCTL_VALUE (LCD_MADCTL | (LCD_BGR ? 0x08 : 0x00))

// Standard ST7735R bring-up (same sequence the common libraries use).
static const init_step_t k_init[] = {
    {0x01, 0, {0}, 150},                                  // software reset
    {0x11, 0, {0}, 500},                                  // sleep out
    {0xB1, 3, {0x01, 0x2C, 0x2D}, 0},                     // frame rate, normal mode
    {0xB2, 3, {0x01, 0x2C, 0x2D}, 0},                     // idle mode
    {0xB3, 6, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 0},   // partial mode
    {0xB4, 1, {0x07}, 0},                                 // inversion control
    {0xC0, 3, {0xA2, 0x02, 0x84}, 0},                     // power control 1
    {0xC1, 1, {0xC5}, 0},
    {0xC2, 2, {0x0A, 0x00}, 0},
    {0xC3, 2, {0x8A, 0x2A}, 0},
    {0xC4, 2, {0x8A, 0xEE}, 0},
    {0xC5, 1, {0x0E}, 0},                                 // VCOM
    {LCD_INVERT ? 0x21 : 0x20, 0, {0}, 0},                // inversion on/off
    {0x36, 1, {MADCTL_VALUE}, 0},                         // orientation + RGB/BGR
    {0x3A, 1, {0x05}, 0},                                 // 16-bit colour
    {0xE0, 16, {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10}, 0},
    {0xE1, 16, {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10}, 0},
    {0x13, 0, {0}, 10},                                   // normal display on
    {0x29, 0, {0}, 100},                                  // display on
};

esp_err_t tft_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_DC) | (1ULL << PIN_TFT_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    if (PIN_TFT_BL >= 0) {             // optional GPIO-controlled backlight; off until the first frame
        gpio_set_direction((gpio_num_t)PIN_TFT_BL, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)PIN_TFT_BL, 0);
    }

    const spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_TFT_CS,
        .queue_size = 4,
        .pre_cb = dc_pre_cb,
    };
    esp_err_t err = spi_bus_add_device(BOARD_SPI_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(PIN_TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        cmd_data(k_init[i].cmd, k_init[i].d, k_init[i].n);
        if (k_init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(k_init[i].delay_ms));
    }
    ESP_LOGI(TAG, "ST7735 ready: %dx%d, MADCTL 0x%02X, %s, invert %d, offset %d,%d, %d MHz",
             GFX_W, GFX_H, MADCTL_VALUE, LCD_BGR ? "BGR" : "RGB", LCD_INVERT,
             LCD_XOFFSET, LCD_YOFFSET, LCD_SPI_HZ / 1000000);
    return ESP_OK;
}

void tft_backlight(bool on)
{
    if (PIN_TFT_BL >= 0) gpio_set_level((gpio_num_t)PIN_TFT_BL, on ? 1 : 0);
}

void tft_write_rows(int y0, int y1, const uint16_t *fb)
{
    uint8_t d[4];
    const int xs = LCD_XOFFSET, xe = LCD_XOFFSET + GFX_W - 1;
    const int ys = LCD_YOFFSET + y0, ye = LCD_YOFFSET + y1;
    d[0] = xs >> 8; d[1] = xs & 0xFF; d[2] = xe >> 8; d[3] = xe & 0xFF;
    cmd_data(0x2A, d, 4);
    d[0] = ys >> 8; d[1] = ys & 0xFF; d[2] = ye >> 8; d[3] = ye & 0xFF;
    cmd_data(0x2B, d, 4);
    cmd(0x2C);

    const size_t row_bytes = GFX_W * 2;
    const int rows_per = SPI_BUS_MAX_XFER_BYTES / row_bytes;
    for (int y = y0; y <= y1; y += rows_per) {
        int n = (y1 - y + 1) < rows_per ? (y1 - y + 1) : rows_per;
        tx(fb + (size_t)y * GFX_W, (size_t)n * row_bytes, 1);
    }
}
