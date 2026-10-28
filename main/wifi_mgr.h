#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

// Station mode with automatic reconnect, plus IST time from NTP.
esp_err_t wifi_mgr_init(const char *ssid, const char *pass);   // empty ssid = stay idle
void wifi_mgr_apply(const char *ssid, const char *pass);       // new credentials, reconnect now
bool wifi_mgr_has_creds(void);
bool wifi_mgr_connected(void);
int  wifi_mgr_rssi(void);                                      // dBm, 0 when not connected

bool time_is_valid(void);
bool time_get_local(struct tm *out);                           // IST
void time_set_epoch(int64_t epoch_utc);                        // from the admin script; NTP overrides later
