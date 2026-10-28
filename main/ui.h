#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_CALIBRATE,    // panel test pattern (Stage A only)
    UI_SPLASH,
    UI_NORMAL,       // ready: class/section + tap card
    UI_COUNTDOWN,
    UI_RECORDING,
    UI_SAVING,
    UI_UPLOADING,
    UI_ADMIN,
    UI_ERROR,
    UI_INFO,         // green confirmation (e.g. recording saved)
} ui_screen_t;

enum { UI_UNKNOWN = 0, UI_OK = 1, UI_FAIL = 2 };

typedef struct {
    ui_screen_t screen;

    // header (shown on NORMAL, COUNTDOWN, RECORDING, SAVING, UPLOADING)
    bool time_valid;
    int  wday;            // 0 = Sunday
    int  day, mon;        // mon 1..12
    int  hour, min;
    int  wifi_rssi;       // dBm, 0 = not connected

    // footer status: UI_UNKNOWN / UI_OK / UI_FAIL
    uint8_t mic, wifi, sd, nfc;

    char class_name[16];
    char section[8];
    char teacher[32];
    uint32_t rec_seconds;
    int  countdown;       // 3, 2, 1
    bool show_stop_hint;  // another card tapped while recording (also: "please wait" banner while uploading)
    int  progress;        // uploading: 0..100, -1 = no bar
    uint32_t up_done_kb, up_total_kb;   // uploading: file bytes sent / size
    char up_status[28];   // uploading: short phase text ("Uploading...", "Retry 2/3...")
    char ble_name[24];
    char err_title[20];
    char err_line1[40];
    char err_line2[40];
} ui_model_t;

// Redraws the whole screen into the framebuffer and flushes it to the panel.
void ui_render(const ui_model_t *m);
