// Stage A: show every screen with fake data. Needs only the TFT wired up.
// BOOT button = skip to the next screen. Check the first screen (panel test)
// and tune LCD_* in app_config.h until it looks right.
#include "stages.h"
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "sd_card.h"      // board_spi_init()
#include "tft.h"
#include "ui.h"

static const char *TAG = "display_demo";

typedef struct {
    ui_screen_t screen;
    int ms;
    const char *name;
    int variant;
} scene_t;

enum { V_NONE, V_UNCONFIGURED, V_HINT, V_ERR_SD, V_ERR_CARD, V_CD3, V_CD2, V_CD1 };

static const scene_t k_scenes[] = {
    {UI_CALIBRATE, 20000, "panel test (press BOOT to continue)", V_NONE},
    {UI_SPLASH,     2000, "splash", V_NONE},
    {UI_NORMAL,     3000, "normal - not set up", V_UNCONFIGURED},
    {UI_NORMAL,     4000, "normal - ready", V_NONE},
    {UI_COUNTDOWN,  1000, "countdown 3", V_CD3},
    {UI_COUNTDOWN,  1000, "countdown 2", V_CD2},
    {UI_COUNTDOWN,  1000, "countdown 1", V_CD1},
    {UI_RECORDING,  6000, "recording", V_NONE},
    {UI_RECORDING,  4000, "recording - other card tapped", V_HINT},
    {UI_SAVING,     2000, "saving", V_NONE},
    {UI_UPLOADING,  4000, "uploading", V_NONE},
    {UI_ADMIN,      4000, "admin mode", V_NONE},
    {UI_ERROR,      3000, "error - SD card", V_ERR_SD},
    {UI_ERROR,      3000, "error - unknown card", V_ERR_CARD},
};

static bool button_down(void) { return gpio_get_level(PIN_BOOT_BTN) == 0; }

void stage_display_demo(void)
{
    ESP_LOGI(TAG, "Stage A - display demo");
    ESP_ERROR_CHECK(board_spi_init());
    ESP_ERROR_CHECK(tft_init());
    if (!gfx_init()) {
        ESP_LOGE(TAG, "framebuffer allocation failed");
        return;
    }
    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    static const int k_rssi[] = {-48, -62, -72, -82, 0};
    const int n_scenes = (int)(sizeof(k_scenes) / sizeof(k_scenes[0]));
    int idx = 0;
    bool was_down = false;

    for (;;) {
        const scene_t *sc = &k_scenes[idx];
        ESP_LOGI(TAG, "screen %d/%d: %s", idx + 1, n_scenes, sc->name);
        const int64_t t0 = esp_timer_get_time();
        bool first = true;
        bool skip = false;

        while (!skip && (esp_timer_get_time() - t0) / 1000 < sc->ms) {
            const int64_t up_s = esp_timer_get_time() / 1000000;
            const int64_t el_ms = (esp_timer_get_time() - t0) / 1000;

            ui_model_t m;
            memset(&m, 0, sizeof(m));
            m.screen = sc->screen;
            m.time_valid = true;
            m.wday = 3; m.day = 14; m.mon = 10;                   // Wed 14 Oct
            const int64_t sec = 10 * 3600 + 24 * 60 + up_s;
            m.hour = (int)((sec / 3600) % 24);
            m.min = (int)((sec / 60) % 60);
            m.wifi_rssi = k_rssi[(up_s / 3) % 5];
            m.mic = UI_OK; m.wifi = m.wifi_rssi ? UI_OK : UI_FAIL; m.sd = UI_OK; m.nfc = UI_OK;
            if (sc->variant != V_UNCONFIGURED) {
                strcpy(m.class_name, "7");
                strcpy(m.section, "B");
            }
            strcpy(m.teacher, "Priya Sharma");
            m.rec_seconds = 754 + (uint32_t)(el_ms / 1000);
            m.show_stop_hint = (sc->variant == V_HINT);
            m.countdown = sc->variant == V_CD3 ? 3 : (sc->variant == V_CD2 ? 2 : 1);
            m.progress = (int)(el_ms * 100 / sc->ms);
            strcpy(m.ble_name, "TCHR-REC-4F2A");
            if (sc->variant == V_ERR_SD) {
                strcpy(m.err_title, "SD CARD");
                strcpy(m.err_line1, "Card missing");
                strcpy(m.err_line2, "Insert and restart");
            } else if (sc->variant == V_ERR_CARD) {
                strcpy(m.err_title, "UNKNOWN CARD");
                strcpy(m.err_line1, "Not registered");
                strcpy(m.err_line2, "UID 04A1B2C3");
            }

            const int64_t r0 = esp_timer_get_time();
            ui_render(&m);
            static bool bl_on = false;
            if (!bl_on) { tft_backlight(true); bl_on = true; }
            if (first) {
                ESP_LOGI(TAG, "  render + push: %lld ms", (long long)((esp_timer_get_time() - r0) / 1000));
                first = false;
            }

            for (int i = 0; i < 10; i++) {                        // ~500 ms, polling the button
                bool down = button_down();
                if (down && !was_down) skip = true;
                was_down = down;
                if (skip) break;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        idx = (idx + 1) % n_scenes;
    }
}
