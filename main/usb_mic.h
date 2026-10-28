#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t  channels;
    uint8_t  bit_resolution;
    uint8_t  bytes_per_sample;
    uint32_t sample_rate;
} mic_format_t;

// Installs USB host + UAC driver. When the mic enumerates, the device is
// opened and its best format is chosen and cached. Plug it in before or after.
esp_err_t usb_mic_init(void);
bool usb_mic_is_connected(void);
bool usb_mic_wait_connected(uint32_t timeout_ms);
esp_err_t usb_mic_get_format(mic_format_t *out);

// Start/stop the isochronous stream on the already-open mic.
esp_err_t usb_mic_start(void);
void usb_mic_stop(void);

// True after a disconnect or a USB transfer error during streaming.
bool usb_mic_has_fault(void);

// Blocking PCM read. Returns ESP_OK with *got > 0 when audio arrived.
esp_err_t usb_mic_read(uint8_t *buf, uint32_t len, uint32_t *got, uint32_t timeout_ms);
