#include "button.h"
#include "app_config.h"
#include "app_events.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";
static volatile bool s_double_enabled;

#define DEBOUNCE_US   30000LL
#define LONG_US       ((int64_t)BTN_LONG_MS * 1000)
#define DOUBLE_GAP_US ((int64_t)BTN_DOUBLE_GAP_MS * 1000)

static bool raw_pressed(void)
{
    if (gpio_get_level(PIN_BTN_STOP) == 0) return true;
#if BTN_USE_BOOT_BUTTON
    if (gpio_get_level(PIN_BOOT_BTN) == 0) return true;
#endif
    return false;
}

static void button_task(void *arg)
{
    bool down = false, raw_prev = false, long_fired = false;
    int clicks = 0;
    int64_t t_down = 0, t_up = 0, raw_change = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int64_t now = esp_timer_get_time();
        const bool raw = raw_pressed();
        if (raw != raw_prev) { raw_prev = raw; raw_change = now; }

        if (raw != down && (now - raw_change) >= DEBOUNCE_US) {
            down = raw;
            if (down) {
                t_down = now;
                long_fired = false;
            } else if (!long_fired) {
                if (s_double_enabled) { clicks++; t_up = now; }
                else { ESP_LOGI(TAG, "single press"); app_post(EVT_BTN_SINGLE); }
            }
        }
        if (down && !long_fired && (now - t_down) >= LONG_US) {
            long_fired = true;
            clicks = 0;
            ESP_LOGI(TAG, "long press");
            app_post(EVT_BTN_LONG);
        }
        if (!down && clicks > 0 && (now - t_up) >= DOUBLE_GAP_US) {
            ESP_LOGI(TAG, "%s", clicks >= 2 ? "double press" : "single press");
            app_post(clicks >= 2 ? EVT_BTN_DOUBLE : EVT_BTN_SINGLE);
            clicks = 0;
        }
    }
}

void button_set_double_enabled(bool on) { s_double_enabled = on; }

void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_STOP)
#if BTN_USE_BOOT_BUTTON
                      | (1ULL << PIN_BOOT_BTN)
#endif
        ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "button on GPIO%d%s", (int)PIN_BTN_STOP, BTN_USE_BOOT_BUTTON ? " (+ BOOT button)" : "");
}
