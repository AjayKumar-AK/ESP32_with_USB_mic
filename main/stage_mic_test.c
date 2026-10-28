// Step 1: USB mic -> (mono, DC-block, FIR, decimate, gain) -> PSRAM ring -> WAV on SD.
// NFC, TFT, stop button, BLE setup and upload come in later steps.
// Bring-up trigger: press the on-board BOOT button to start, press again to stop.
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "board_pins.h"
#include "recorder.h"
#include "sd_card.h"
#include "usb_mic.h"

static const char *TAG = "main";

static void log_result(esp_err_t err, const rec_result_t *r)
{
    ESP_LOGI(TAG, "result: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "file: %s", r->path);
    ESP_LOGI(TAG, "wav: %lu Hz, %u ch, %u bit | mic negotiated %lu Hz, measured %lu Hz",
             (unsigned long)r->sample_rate, r->channels, r->bits,
             (unsigned long)r->native_rate_hz, (unsigned long)r->measured_rate_hz);
    ESP_LOGI(TAG, "audio: %lu bytes, %lu ms, dropped %lu bytes",
             (unsigned long)r->data_bytes, (unsigned long)r->duration_ms,
             (unsigned long)r->dropped_bytes);
}

static void stop_and_report(void)
{
    rec_result_t r;
    esp_err_t err = recorder_stop(&r);
    log_result(err, &r);
}

void stage_mic_test(void)
{
    ESP_LOGI(TAG, "teacher recorder - step 1 (USB mic -> SD)");
    ESP_LOGI(TAG, "free PSRAM %u, free internal %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    ESP_ERROR_CHECK(board_spi_init());
    if (sd_card_mount() != ESP_OK) {
        ESP_LOGE(TAG, "no usable SD card - check card, FAT32 format and wiring");
        return;
    }
    uint64_t total = 0, freeb = 0;
    if (sd_card_free_bytes(&total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "SD: %llu MB total, %llu MB free",
                 (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20));
    }

    ESP_ERROR_CHECK(recorder_init());
    recorder_recover_parts();
    ESP_ERROR_CHECK(usb_mic_init());

    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    ESP_LOGI(TAG, "ready: plug in the mic, then press BOOT to start/stop a test recording");

    bool was_down = false;
    for (;;) {
        bool down = gpio_get_level(PIN_BOOT_BTN) == 0;
        if (down && !was_down) {
            if (recorder_state() == REC_IDLE) {
                char name[48];
                snprintf(name, sizeof(name), "TEST_%llu",
                         (unsigned long long)(esp_timer_get_time() / 1000000));
                esp_err_t err = recorder_start(name);
                if (err != ESP_OK) ESP_LOGE(TAG, "recorder_start: %s", esp_err_to_name(err));
            } else if (recorder_state() == REC_RECORDING) {
                stop_and_report();
            }
        }
        was_down = down;

        if (recorder_needs_stop()) {
            ESP_LOGW(TAG, "recording aborted by a fault - saving what was captured");
            stop_and_report();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
