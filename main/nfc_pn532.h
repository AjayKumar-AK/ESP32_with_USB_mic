#pragma once
#include <stdbool.h>
#include "esp_err.h"

// PN532 on I2C (mode switches set to I2C). Polls for ISO14443A cards and posts
// EVT_TAP once per tap: a card must leave the field for NFC_RELEASE_MS before the next tap counts.
esp_err_t nfc_init(void);        // ESP_OK when the PN532 answered
void nfc_start_task(void);       // polling task; retries init every 3 s if the reader is missing
bool nfc_ok(void);
