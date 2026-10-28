// The real firmware: NFC card -> countdown -> recording -> SD, admin mode over BLE,
// real IST clock and Wi-Fi signal in the header. Upload comes later.
#include "stages.h"
#include <stdio.h>
#include "app_config.h"
#include "app_ctrl.h"
#include "app_events.h"
#include "admin_ble.h"
#include "button.h"
#include "config_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gfx.h"
#include "nfc_pn532.h"
#include "recorder.h"
#include "sd_card.h"
#include "sd_log.h"
#include "teachers.h"
#include "tft.h"
#include "uploader.h"
#include "ui.h"
#include "usb_mic.h"
#include "wifi_mgr.h"

static const char *TAG = "boot";

static void heap_log(const char *where)
{
    ESP_LOGI(TAG, "heap after %-10s internal %u KB, psram %u KB", where,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void stage_full_app(void)
{
    ESP_LOGI(TAG, "teacher recorder firmware %s", FW_VERSION);
    heap_log("start");

    app_events_init();
    ESP_ERROR_CHECK(config_init());
    const uint32_t boot = config_next_boot_count();
    sd_log_init(boot);                       // from here on every log line is also saved to /sdcard/logs
    app_config_t cfg;
    config_load(&cfg);
    teachers_init();

    ESP_ERROR_CHECK(board_spi_init());
    bool display = (tft_init() == ESP_OK) && gfx_init();
    if (display) {
        ui_model_t m = { .screen = UI_SPLASH };
        ui_render(&m);
        tft_backlight(true);
    } else {
        ESP_LOGE(TAG, "display failed - continuing without it");
    }
    heap_log("display");

    const bool sd = sd_card_mount() == ESP_OK;
    if (sd) {
        uint64_t total = 0, freeb = 0;
        if (sd_card_free_bytes(&total, &freeb) == ESP_OK) {   // also warms FatFS's free-space cache
            ESP_LOGI(TAG, "SD: %llu MB total, %llu MB free",
                     (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20));
        }
    } else {
        ESP_LOGE(TAG, "SD card not available");
    }
    heap_log("sd");

    ESP_ERROR_CHECK(recorder_init());
    if (sd) recorder_recover_parts();
    if (usb_mic_init() != ESP_OK) ESP_LOGE(TAG, "USB mic init failed");
    heap_log("usb");

    uploader_init();
    if (sd) uploader_cleanup_uploaded(SD_KEEP_FREE_BYTES);
    nfc_start_task();                                        // initialises the PN532 itself and retries
    if (wifi_mgr_init(cfg.ssid, cfg.pass) != ESP_OK) ESP_LOGE(TAG, "Wi-Fi start failed");
    heap_log("wifi");
    if (admin_ble_init() != ESP_OK) ESP_LOGE(TAG, "BLE init failed");
    heap_log("ble");
    button_init();

    app_ctrl_run(boot, display);
}
