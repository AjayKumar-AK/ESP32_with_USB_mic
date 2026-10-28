// SD wiring test without the SD driver: bit-bangs SPI-mode CMD0 and CMD8 on the SD pins
// and prints exactly what comes back on MISO. Tells wiring/power faults apart from software.
#include "stages.h"
#include <stdio.h>
#include <string.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdtest";

#define HALF_US 5   // about 100 kHz, slow enough for any wiring

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_SPI_MOSI, (out >> i) & 1);
        esp_rom_delay_us(HALF_US);
        gpio_set_level(PIN_SPI_SCLK, 1);
        esp_rom_delay_us(HALF_US);
        in = (uint8_t)((in << 1) | (gpio_get_level(PIN_SPI_MISO) & 1));
        gpio_set_level(PIN_SPI_SCLK, 0);
    }
    return in;
}

static void command(uint8_t idx, uint32_t arg, uint8_t crc, uint8_t *resp, int resp_len)
{
    xfer(0xFF);
    xfer(0x40 | idx);
    xfer((uint8_t)(arg >> 24)); xfer((uint8_t)(arg >> 16)); xfer((uint8_t)(arg >> 8)); xfer((uint8_t)arg);
    xfer(crc);
    memset(resp, 0xFF, resp_len);
    for (int i = 0; i < 16; i++) {                 // R1 arrives within 8 bytes, MSB clear
        uint8_t b = xfer(0xFF);
        if (!(b & 0x80)) {
            resp[0] = b;
            for (int k = 1; k < resp_len; k++) resp[k] = xfer(0xFF);
            return;
        }
    }
}

// Returns CMD0's R1 byte (0xFF = nothing heard). Optionally the 5-byte CMD8 answer.
static uint8_t run_once(gpio_pull_mode_t pull, uint8_t cmd8[5])
{
    gpio_set_pull_mode(PIN_SPI_MISO, pull);
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(PIN_SPI_MOSI, 1);
    for (int i = 0; i < 80; i++) {                 // 80 clocks with CS high wakes the card
        gpio_set_level(PIN_SPI_SCLK, 1); esp_rom_delay_us(HALF_US);
        gpio_set_level(PIN_SPI_SCLK, 0); esp_rom_delay_us(HALF_US);
    }
    gpio_set_level(PIN_SD_CS, 0);
    uint8_t r1[1];
    command(0, 0, 0x95, r1, 1);                    // CMD0: go idle, expect R1 = 0x01
    if (cmd8) command(8, 0x1AA, 0x87, cmd8, 5);    // CMD8: expect 01 00 00 01 AA
    gpio_set_level(PIN_SD_CS, 1);
    xfer(0xFF);
    return r1[0];
}

void stage_sdtest(void)
{
    const gpio_num_t outs[] = {PIN_SD_CS, PIN_SPI_SCLK, PIN_SPI_MOSI, PIN_TFT_CS};
    for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
        gpio_reset_pin(outs[i]);
        gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
    }
    gpio_set_level(PIN_TFT_CS, 1);                 // keep the display deaf
    gpio_set_level(PIN_SPI_SCLK, 0);
    gpio_reset_pin(PIN_SPI_MISO);
    gpio_set_direction(PIN_SPI_MISO, GPIO_MODE_INPUT);

    ESP_LOGI(TAG, "SD wiring test: CS GPIO%d, MOSI GPIO%d, MISO GPIO%d, SCK GPIO%d",
             (int)PIN_SD_CS, (int)PIN_SPI_MOSI, (int)PIN_SPI_MISO, (int)PIN_SPI_SCLK);
    ESP_LOGI(TAG, "Expected from a healthy card: CMD0 -> 0x01, CMD8 -> 01 00 00 01 AA");

    for (;;) {
        uint8_t c8[5];
        uint8_t up = run_once(GPIO_PULLUP_ONLY, c8);
        uint8_t dn = run_once(GPIO_PULLDOWN_ONLY, NULL);
        ESP_LOGI(TAG, "CMD0 reply with MISO pull-up: 0x%02X | pull-down: 0x%02X | CMD8: %02X %02X %02X %02X %02X",
                 up, dn, c8[0], c8[1], c8[2], c8[3], c8[4]);

        if (up == 0x01 || dn == 0x01) {
            ESP_LOGI(TAG, "=> CARD ANSWERS. Wiring to the card works; if mounting still fails, tell me this line.");
        } else if (up == 0xFF && dn == 0xFF) {
            ESP_LOGW(TAG, "=> Silence on MISO (reads 0xFF either way). Card unpowered or wrong supply voltage, "
                          "CS wire not reaching the module, or the MISO wire is connected but the card is not talking.");
        } else if (up != dn) {
            ESP_LOGW(TAG, "=> MISO is FLOATING (value follows the pull resistor): nothing is driving it. "
                          "Check the MISO wire (GPIO%d), card power and that the card is seated.", (int)PIN_SPI_MISO);
        } else if (up == 0x00) {
            ESP_LOGW(TAG, "=> MISO stuck LOW: short to GND, wrong pin, or the module is holding it low.");
        } else {
            ESP_LOGW(TAG, "=> Unexpected reply 0x%02X: card present but unhappy (power sag, long wires, wrong voltage).", up);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
