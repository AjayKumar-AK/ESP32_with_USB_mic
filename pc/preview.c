#include "gfx.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
extern uint16_t pc_screen[];

static void dump(const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), "/home/claude/prev/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", GFX_W, GFX_H);
    for (int i = 0; i < GFX_W * GFX_H; i++) {
        uint16_t v = (uint16_t)((pc_screen[i] >> 8) | (pc_screen[i] << 8));
        fputc(((v >> 11) & 31) * 255 / 31, f);
        fputc(((v >> 5) & 63) * 255 / 63, f);
        fputc((v & 31) * 255 / 31, f);
    }
    fclose(f);
}

int main(void)
{
    gfx_init();
    ui_model_t b;
    memset(&b, 0, sizeof(b));
    b.time_valid = true; b.wday = 3; b.day = 14; b.mon = 10; b.hour = 10; b.min = 24;
    b.wifi_rssi = -58;
    b.mic = b.wifi = b.sd = b.nfc = UI_OK;
    strcpy(b.class_name, "7"); strcpy(b.section, "B");
    strcpy(b.teacher, "Priya Sharma");
    b.rec_seconds = 754; b.progress = 62; b.countdown = 3;
    strcpy(b.ble_name, "TCHR-REC-4F2A");

    ui_model_t m;
#define SHOT(nm, setup) do { m = b; setup; ui_render(&m); dump(nm); } while (0)
    SHOT("01_calibrate", m.screen = UI_CALIBRATE);
    SHOT("02_splash", m.screen = UI_SPLASH);
    SHOT("03_normal", m.screen = UI_NORMAL);
    SHOT("04_not_set_up", m.screen = UI_NORMAL; m.class_name[0] = 0);
    SHOT("05_countdown", m.screen = UI_COUNTDOWN);
    SHOT("06_recording", m.screen = UI_RECORDING);
    SHOT("07_stop_hint", m.screen = UI_RECORDING; m.show_stop_hint = true);
    SHOT("08_long_name", m.screen = UI_RECORDING; strcpy(m.teacher, "Dr. Venkatasubramanian Iyer-K"));
    SHOT("09_saving", m.screen = UI_SAVING);
    SHOT("10_uploading", m.screen = UI_UPLOADING; strcpy(m.up_status, "Uploading..."); m.up_done_kb = 12800; m.up_total_kb = 46700; m.progress = 27);
    SHOT("10b_upload_wait", m.screen = UI_UPLOADING; strcpy(m.up_status, "Uploading..."); m.up_done_kb = 12800; m.up_total_kb = 46700; m.progress = 27; m.show_stop_hint = true);
    SHOT("10c_upload_start", m.screen = UI_UPLOADING; strcpy(m.up_status, "Starting session..."); m.progress = -1);
    SHOT("11_admin", m.screen = UI_ADMIN);
    SHOT("12_error_sd", m.screen = UI_ERROR; strcpy(m.err_title, "SD CARD"); strcpy(m.err_line1, "Card missing"); strcpy(m.err_line2, "Insert and restart"));
    SHOT("14_saved", m.screen = UI_INFO; strcpy(m.err_title, "SAVED"); strcpy(m.err_line1, "Priya Sharma"); strcpy(m.err_line2, "00:12:34"));
    SHOT("13_no_wifi_no_time", m.screen = UI_NORMAL; m.time_valid = false; m.wifi_rssi = 0; m.wifi = UI_FAIL);
    return 0;
}
