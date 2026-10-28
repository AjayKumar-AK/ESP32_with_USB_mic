// Wiring test for the TFT: drives each display line high/low slowly so you can
// check with a multimeter (DC volts, black probe on GND) that it reaches the
// matching pin on the display module. No SPI, no display driver involved.
#include "stages.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pintest";

typedef struct { const char *label; const char *module_pin; gpio_num_t gpio; } line_t;

void stage_pintest(void)
{
    static const line_t lines[] = {
        {"CS",   "CS",  PIN_TFT_CS},
        {"DC",   "A0",  PIN_TFT_DC},
        {"RST",  "RST", PIN_TFT_RST},
        {"MOSI", "SDA", PIN_SPI_MOSI},
        {"SCK",  "SCL", PIN_SPI_SCLK},
    };
    const int n = sizeof(lines) / sizeof(lines[0]);

    for (int i = 0; i < n; i++) {
        gpio_reset_pin(lines[i].gpio);
        gpio_set_direction(lines[i].gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(lines[i].gpio, 0);
    }
    ESP_LOGI(TAG, "Measure between the display pin and GND: HIGH = about 3.3 V, LOW = about 0 V");

    for (;;) {
        for (int i = 0; i < n; i++) {
            for (int level = 1; level >= 0; level--) {
                gpio_set_level(lines[i].gpio, level);
                ESP_LOGI(TAG, "%-4s GPIO%-2d -> display pin %-3s : %s",
                         lines[i].label, (int)lines[i].gpio, lines[i].module_pin,
                         level ? "HIGH (3.3 V)" : "LOW (0 V)");
                vTaskDelay(pdMS_TO_TICKS(4000));
            }
            gpio_set_level(lines[i].gpio, 0);
        }
        ESP_LOGI(TAG, "--- round complete, repeating ---");
    }
}
