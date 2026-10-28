#include "ui.h"
#include "gfx.h"
#include "font.h"
#include <stdio.h>
#include <string.h>

#define C_BG    GFX_RGB(8, 16, 64)
#define C_HDR   GFX_RGB(22, 36, 112)
#define C_PILL  GFX_RGB(30, 52, 150)
#define C_TEXT  GFX_RGB(255, 255, 255)
#define C_DIM   GFX_RGB(150, 160, 210)
#define C_DIM2  GFX_RGB(70, 80, 130)
#define C_AMBER GFX_RGB(255, 176, 0)
#define C_OK    GFX_RGB(40, 200, 90)
#define C_BAD   GFX_RGB(235, 60, 50)
#define C_REC   GFX_RGB(238, 64, 48)
#define C_DARK  GFX_RGB(20, 20, 40)
#define C_GREY  GFX_RGB(110, 115, 140)

#define HDR_H   16
#define FOOT_Y  108

static const char *k_wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *k_mon[]  = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void draw_wifi(int x, int yb, int rssi)
{
    int bars = 0;
    if (rssi != 0) {
        if (rssi > -55) bars = 4;
        else if (rssi > -65) bars = 3;
        else if (rssi > -75) bars = 2;
        else if (rssi > -85) bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int h = 3 + 2 * i;
        gfx_fill_rect(x + i * 5, yb - h + 1, 3, h, i < bars ? C_TEXT : C_DIM2);
    }
    if (rssi == 0) gfx_fill_circle(x + 23, yb - 3, 2, C_BAD);
}

static void header(const ui_model_t *m)
{
    gfx_fill_rect(0, 0, GFX_W, HDR_H, C_HDR);
    char buf[32];
    if (m->time_valid) {
        snprintf(buf, sizeof(buf), "%s %d %s  %02d:%02d",
                 k_wday[m->wday % 7], m->day, k_mon[(m->mon + 11) % 12], m->hour, m->min);
        gfx_text(&font_sm, 4, (HDR_H - font_sm.height) / 2, buf, C_TEXT);
    } else {
        gfx_text(&font_sm, 4, (HDR_H - font_sm.height) / 2, "--:--", C_DIM);
    }
    draw_wifi(130, 12, m->wifi_rssi);
}

static void dot_label(int x, uint8_t state, const char *label)
{
    color_t c = state == UI_OK ? C_OK : (state == UI_FAIL ? C_BAD : C_GREY);
    gfx_fill_circle(x + 6, FOOT_Y + 10, 3, c);
    gfx_text(&font_sm, x + 12, FOOT_Y + (GFX_H - FOOT_Y - font_sm.height) / 2, label, C_DIM);
}

static void footer(const ui_model_t *m)
{
    gfx_fill_rect(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y, C_HDR);
    dot_label(2,   m->mic,  "Mic");
    dot_label(42,  m->wifi, "WiFi");
    dot_label(88,  m->sd,   "SD");
    dot_label(118, m->nfc,  "NFC");
}

static void class_line(const ui_model_t *m, char *buf, size_t n)
{
    snprintf(buf, n, "Class %s - %s", m->class_name, m->section);
}

/* --------------------------------------------------------------- screens */
static void scr_calibrate(void)
{
    gfx_fill(GFX_RGB(0, 0, 0));
    const color_t w = GFX_RGB(255, 255, 255);
    gfx_fill_rect(0, 0, GFX_W, 1, w);
    gfx_fill_rect(0, GFX_H - 1, GFX_W, 1, w);
    gfx_fill_rect(0, 0, 1, GFX_H, w);
    gfx_fill_rect(GFX_W - 1, 0, 1, GFX_H, w);

    gfx_text(&font_smb, 4, 3, "TL", w);
    int brw = gfx_text_width(&font_smb, "BR");
    gfx_text(&font_smb, GFX_W - brw - 4, GFX_H - font_smb.height - 3, "BR", w);

    gfx_fill_rect(8, 22, 48, 40, GFX_RGB(255, 0, 0));
    gfx_fill_rect(56, 22, 48, 40, GFX_RGB(0, 255, 0));
    gfx_fill_rect(104, 22, 48, 40, GFX_RGB(0, 0, 255));
    gfx_text_center(&font_md, 32, 42, "R", w);
    gfx_text_center(&font_md, 80, 42, "G", GFX_RGB(0, 0, 0));
    gfx_text_center(&font_md, 128, 42, "B", w);

    for (int i = 0; i < 8; i++) {
        int v = i * 255 / 7;
        gfx_fill_rect(8 + i * 18, 68, 18, 14, GFX_RGB(v, v, v));
    }
    gfx_text_center(&font_sm, 80, 94, "160x128  border intact?", w);
    gfx_text_center(&font_sm, 80, 108, "R/G/B right? not inverted?", GFX_RGB(150, 150, 150));
}

static void scr_splash(void)
{
    gfx_fill(C_BG);
    gfx_text_center(&font_lg, 80, 52, "Teacher", C_TEXT);
    gfx_text_center(&font_lg, 80, 76, "Recorder", C_AMBER);
    gfx_text_center(&font_sm, 80, 108, "starting...", C_DIM);
}

static void scr_normal(const ui_model_t *m)
{
    gfx_fill(C_BG);
    header(m);
    footer(m);
    if (m->class_name[0] == 0) {
        gfx_text_center(&font_lg, 80, 44, "Not set up", C_AMBER);
        gfx_text_center(&font_md, 80, 70, "Hold button for", C_TEXT);
        gfx_text_center(&font_md, 80, 86, "admin mode", C_TEXT);
        return;
    }
    char buf[48];
    class_line(m, buf, sizeof(buf));
    gfx_text_center(&font_sm, 80, 28, "CLASS / SECTION", C_DIM);
    const font_t *fonts[] = {&font_lg, &font_md};
    gfx_text_fit(fonts, 2, 80, 48, 148, buf, C_TEXT);
    gfx_fill_round_rect_border(12, 66, 136, 32, 10, C_AMBER, C_PILL);
    const font_t *tap[] = {&font_md, &font_smb};
    gfx_text_fit(tap, 2, 80, 82, 124, "TAP CARD TO SCAN", C_TEXT);
}

static void scr_countdown(const ui_model_t *m)
{
    gfx_fill(C_BG);
    header(m);
    footer(m);
    gfx_text_center(&font_smb, 80, 26, "Get ready", C_DIM);
    char d[4];
    snprintf(d, sizeof(d), "%d", m->countdown);
    gfx_text_center(&font_xl, 80, 70, d, C_AMBER);
}

static void timer_pill(int y, int h, uint32_t secs)
{
    char t[16];
    snprintf(t, sizeof(t), "%02u:%02u:%02u", (unsigned)(secs / 3600),
             (unsigned)((secs / 60) % 60), (unsigned)(secs % 60));
    gfx_fill_round_rect(10, y, 140, h, 12, C_REC);
    gfx_fill_circle(28, y + h / 2, 5, (secs & 1) ? C_DIM : C_TEXT);   // blinking REC dot
    gfx_text_center(&font_lg, 94, y + h / 2, t, C_TEXT);
}

static void scr_recording(const ui_model_t *m)
{
    gfx_fill(C_BG);
    header(m);
    footer(m);
    const font_t *fonts[] = {&font_lg, &font_md, &font_smb};
    if (!m->show_stop_hint) {
        gfx_text_fit(fonts, 3, 80, 33, 148, m->teacher, C_TEXT);
        char buf[48];
        class_line(m, buf, sizeof(buf));
        gfx_text_center(&font_sm, 80, 52, buf, C_DIM);
        timer_pill(64, 36, m->rec_seconds);
    } else {
        gfx_text_fit(fonts, 3, 80, 29, 148, m->teacher, C_TEXT);
        timer_pill(42, 30, m->rec_seconds);
        gfx_fill_round_rect(8, 76, 144, 30, 8, C_AMBER);
        gfx_text_center(&font_smb, 80, 85, "Press button to stop", C_DARK);
        gfx_text_center(&font_smb, 80, 98, "current recording", C_DARK);
    }
}

static void scr_saving(const ui_model_t *m)
{
    gfx_fill(C_BG);
    header(m);
    footer(m);
    gfx_text_center(&font_lg, 80, 56, "Saving...", C_TEXT);
    gfx_text_center(&font_sm, 80, 80, "Please wait", C_DIM);
}

static void scr_uploading(const ui_model_t *m)
{
    gfx_fill(C_BG);
    header(m);
    footer(m);
    const font_t *fonts[] = {&font_md, &font_smb};
    gfx_text_fit(fonts, 2, 80, 28, 148, m->teacher[0] ? m->teacher : "Recording", C_TEXT);
    if (m->class_name[0]) {
        char buf[48];
        class_line(m, buf, sizeof(buf));
        gfx_text_center(&font_sm, 80, 44, buf, C_DIM);
    }
    gfx_text_center(&font_smb, 80, 60, m->up_status[0] ? m->up_status : "Uploading...", C_AMBER);

    const int p = m->progress > 100 ? 100 : m->progress;
    gfx_fill_round_rect_border(14, 70, 132, 14, 5, C_DIM, C_BG);
    if (p > 0) gfx_fill_round_rect(16, 72, (128 * p) / 100, 10, 4, C_OK);

    if (m->show_stop_hint) {
        gfx_fill_round_rect(10, 89, 140, 17, 6, C_AMBER);
        gfx_text_center(&font_smb, 80, 97, "Please wait, uploading", C_DARK);
    } else if (m->up_total_kb > 0) {
        char t[40];
        const unsigned d10 = (m->up_done_kb * 10u / 1024u) % 100000u;     // tenths of a MB, bounded
        const unsigned t10 = (m->up_total_kb * 10u / 1024u) % 100000u;
        snprintf(t, sizeof(t), "%u.%u / %u.%u MB  %d%%", d10 / 10, d10 % 10, t10 / 10, t10 % 10, p < 0 ? 0 : p);
        gfx_text_center(&font_sm, 80, 97, t, C_DIM);
    }
}

static void scr_admin(const ui_model_t *m)
{
    gfx_fill(C_BG);
    gfx_fill_rect(0, 0, GFX_W, 20, C_AMBER);
    gfx_text_center(&font_md, 80, 10, "ADMIN MODE", C_DARK);
    gfx_text_center(&font_md, 80, 36, "Connect to admin", C_TEXT);
    gfx_text_center(&font_smb, 80, 54, "or double press to exit", C_TEXT);
    gfx_text_center(&font_sm, 80, 74, "Bluetooth name", C_DIM);
    gfx_fill_round_rect_border(10, 84, 140, 30, 8, C_AMBER, C_PILL);
    const font_t *fonts[] = {&font_md, &font_smb};
    gfx_text_fit(fonts, 2, 80, 99, 128, m->ble_name, C_AMBER);
}

static void scr_error(const ui_model_t *m)
{
    gfx_fill(C_BG);
    gfx_fill_rect(0, 0, GFX_W, 22, C_BAD);
    gfx_text_center(&font_md, 80, 11, m->err_title[0] ? m->err_title : "ERROR", C_TEXT);
    const font_t *fonts[] = {&font_md, &font_smb};
    gfx_text_fit(fonts, 2, 80, 52, 150, m->err_line1, C_TEXT);
    gfx_text_fit(fonts, 2, 80, 76, 150, m->err_line2, C_DIM);
}

static void scr_info(const ui_model_t *m)
{
    gfx_fill(C_BG);
    gfx_fill_rect(0, 0, GFX_W, 22, C_OK);
    gfx_text_center(&font_md, 80, 11, m->err_title[0] ? m->err_title : "DONE", C_DARK);
    const font_t *fonts[] = {&font_md, &font_smb};
    gfx_text_fit(fonts, 2, 80, 52, 150, m->err_line1, C_TEXT);
    gfx_text_fit(fonts, 2, 80, 76, 150, m->err_line2, C_DIM);
}

void ui_render(const ui_model_t *m)
{
    switch (m->screen) {
    case UI_CALIBRATE: scr_calibrate(); break;
    case UI_SPLASH:    scr_splash(); break;
    case UI_NORMAL:    scr_normal(m); break;
    case UI_COUNTDOWN: scr_countdown(m); break;
    case UI_RECORDING: scr_recording(m); break;
    case UI_SAVING:    scr_saving(m); break;
    case UI_UPLOADING: scr_uploading(m); break;
    case UI_ADMIN:     scr_admin(m); break;
    case UI_ERROR:     scr_error(m); break;
    case UI_INFO:      scr_info(m); break;
    }
    gfx_flush();
}
