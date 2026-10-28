#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// BLE is initialised once at boot but only advertises while admin mode is on.
esp_err_t admin_ble_init(void);
const char *admin_ble_name(void);             // "TCHR-REC-XXXX", shown on the admin screen
void admin_ble_start(void);                   // enter admin mode: start advertising
void admin_ble_stop(void);                    // leave admin mode: stop advertising, drop any connection
void admin_ble_notify_tap(const char *uid);   // forward a card tap to the connected PC script
uint32_t admin_ble_idle_ms(void);             // time since the last BLE activity
