#include "app_ctrl.h"
#include "admin_ble.h"
#include "app_config.h"
#include "app_events.h"
#include "button.h"
#include "config_store.h"
#include "nfc_pn532.h"
#include "recorder.h"
#include "sd_card.h"
#include "teachers.h"
#include "ui.h"
#include "uploader.h"
#include "usb_mic.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app";

typedef enum {
    ST_NORMAL, ST_COUNTDOWN, ST_RECORDING, ST_SAVING, ST_UPLOADING, ST_ADMIN, ST_NOTICE
} app_state_t;

static QueueHandle_t s_q;
static app_state_t s_state = ST_NORMAL;
static app_config_t s_cfg;
static uint32_t s_boot;
static bool s_display;
static teacher_t s_active;                       // teacher of the current session
static char s_base[64];                          // base name of the file being recorded / uploaded
static bool s_up_retry;                          // current upload is a background retry of an old file
static char s_deferred_uid[24];                  // card tapped while a retry upload was running
static int64_t s_since_us, s_rec_start_us, s_hint_until_us, s_notice_until_us;
static int64_t s_next_space_us, s_next_heap_us, s_next_pending_us, s_next_sd_retry_us;
static bool s_notice_error;
static char s_n_title[20], s_n_l1[40], s_n_l2[40];
static ui_model_t s_prev;
static bool s_prev_valid;

static inline int64_t now_us(void) { return esp_timer_get_time(); }

/* ------------------------------------------------------------- events */
void app_events_init(void) { s_q = xQueueCreate(16, sizeof(app_event_t)); }

bool app_post_event(const app_event_t *e)
{
    return s_q && xQueueSend(s_q, e, 0) == pdTRUE;
}

/* ------------------------------------------------------------ display */
static void fill_model(ui_model_t *m)
{
    memset(m, 0, sizeof(*m));
    struct tm tm;
    if (time_get_local(&tm)) {
        m->time_valid = true;
        m->wday = tm.tm_wday;
        m->day = tm.tm_mday;
        m->mon = tm.tm_mon + 1;
        m->hour = tm.tm_hour;
        m->min = tm.tm_min;
    }
    m->wifi_rssi = wifi_mgr_rssi();
    m->mic  = usb_mic_is_connected() ? UI_OK : UI_FAIL;
    m->wifi = wifi_mgr_connected() ? UI_OK : (wifi_mgr_has_creds() ? UI_FAIL : UI_UNKNOWN);
    m->sd   = sd_card_is_mounted() ? UI_OK : UI_FAIL;
    m->nfc  = nfc_ok() ? UI_OK : UI_FAIL;
    strlcpy(m->class_name, s_cfg.class_name, sizeof(m->class_name));
    strlcpy(m->section, s_cfg.section, sizeof(m->section));
    strlcpy(m->teacher, s_active.name, sizeof(m->teacher));
    strlcpy(m->ble_name, admin_ble_name(), sizeof(m->ble_name));
    m->progress = -1;

    const int64_t now = now_us();
    switch (s_state) {
    case ST_NORMAL:
        m->screen = UI_NORMAL;
        break;
    case ST_COUNTDOWN: {
        m->screen = UI_COUNTDOWN;
        int left = COUNTDOWN_SECONDS - (int)((now - s_since_us) / 1000000);
        m->countdown = left < 1 ? 1 : left;
        break;
    }
    case ST_RECORDING:
        m->screen = UI_RECORDING;
        m->rec_seconds = (uint32_t)((now - s_rec_start_us) / 1000000);
        m->show_stop_hint = now < s_hint_until_us;
        break;
    case ST_SAVING:
        m->screen = UI_SAVING;
        break;
    case ST_UPLOADING: {
        upload_progress_t p;
        uploader_get_progress(&p);
        m->screen = UI_UPLOADING;
        strlcpy(m->teacher, p.teacher, sizeof(m->teacher));
        strlcpy(m->up_status, p.status, sizeof(m->up_status));
        m->up_done_kb = p.done_bytes >> 10;
        m->up_total_kb = p.total_bytes >> 10;
        m->progress = p.total_bytes ? (int)((uint64_t)p.done_bytes * 100 / p.total_bytes) : -1;
        m->show_stop_hint = now < s_hint_until_us;
        break;
    }
    case ST_ADMIN:
        m->screen = UI_ADMIN;
        break;
    case ST_NOTICE:
        m->screen = s_notice_error ? UI_ERROR : UI_INFO;
        strlcpy(m->err_title, s_n_title, sizeof(m->err_title));
        strlcpy(m->err_line1, s_n_l1, sizeof(m->err_line1));
        strlcpy(m->err_line2, s_n_l2, sizeof(m->err_line2));
        break;
    }
}

static void render(bool force)
{
    if (!s_display) return;
    ui_model_t m;
    fill_model(&m);
    if (!force && s_prev_valid && memcmp(&m, &s_prev, sizeof(m)) == 0) return;
    ui_render(&m);
    memcpy(&s_prev, &m, sizeof(m));
    s_prev_valid = true;
}

static void notice(bool is_error, const char *title, const char *l1, const char *l2)
{
    s_notice_error = is_error;
    strlcpy(s_n_title, title, sizeof(s_n_title));
    strlcpy(s_n_l1, l1, sizeof(s_n_l1));
    strlcpy(s_n_l2, l2, sizeof(s_n_l2));
    s_state = ST_NOTICE;
    s_notice_until_us = now_us() + (int64_t)(is_error ? ERROR_SHOW_MS : INFO_SHOW_MS) * 1000;
    if (is_error) ESP_LOGW(TAG, "notice: %s | %s | %s", title, l1, l2);
    else ESP_LOGI(TAG, "notice: %s | %s | %s", title, l1, l2);
    render(true);
}

/* -------------------------------------------------------------- upload */
static bool start_upload(const char *base, const char *uid, bool retry)
{
    if (!uploader_submit(base, uid, retry)) return false;
    s_up_retry = retry;
    s_hint_until_us = 0;
    s_state = ST_UPLOADING;
    render(true);
    return true;
}

static void start_session(const char *uid);

static void poll_upload(void)
{
    upload_result_t r;
    if (!uploader_take_result(&r)) return;
    const int64_t now = now_us();
    ESP_LOGI(TAG, "upload finished: %s (%s) %s", r.base, r.ok ? "ok" : r.msg, r.session);

    if (r.ok) {
        s_next_pending_us = now + 3 * 1000000LL;                 // look for more pending files soon
        upload_progress_t p;
        uploader_get_progress(&p);
        notice(false, "UPLOADED", p.teacher[0] ? p.teacher : r.base, r.session[0] ? r.session : "Done");
    } else if (r.cancelled) {
        s_state = ST_NORMAL;
        render(true);
    } else {
        s_next_pending_us = now + (int64_t)UPLOAD_RETRY_INTERVAL_MS * 1000;
        notice(true, "UPLOAD FAILED", r.msg, "Saved on SD, will retry");
    }

    if (s_deferred_uid[0]) {                                     // a teacher tapped during a background retry
        char uid[24];
        strlcpy(uid, s_deferred_uid, sizeof(uid));
        s_deferred_uid[0] = 0;
        s_state = ST_NORMAL;
        start_session(uid);
    }
}

/* ----------------------------------------------------------- recording */
static void make_base_name(const teacher_t *t, char *out, size_t n)
{
    const char *id = t->id[0] ? t->id : t->uid;
    struct tm tm;
    if (time_get_local(&tm)) {
        snprintf(out, n, "%s_%04u%02u%02u_%02u%02u%02u", id,
                 (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u,
                 (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
                 (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u);
    } else {
        snprintf(out, n, "%s_boot%lu_t%lu", id, (unsigned long)s_boot,
                 (unsigned long)(now_us() / 1000000));
    }
}

// 0 = enough space, 1 = card missing, 2 = card unreadable, 3 = low on space
static int sd_check(uint64_t need, uint64_t *free_out)
{
    if (!sd_card_is_mounted()) return 1;
    uint64_t total = 0, freeb = 0;
    if (sd_card_free_bytes(&total, &freeb) != ESP_OK) return 2;
    if (free_out) *free_out = freeb;
    return freeb >= need ? 0 : 3;
}

static const char *fault_text(esp_err_t e)
{
    switch (e) {
    case ESP_ERR_NOT_FOUND:     return "Mic disconnected";
    case ESP_ERR_TIMEOUT:       return "No audio from mic";
    case ESP_ERR_INVALID_SIZE:  return "File size limit";
    case ESP_FAIL:              return "SD write error";
    default:                    return esp_err_to_name(e);
    }
}

static void stop_recording(const char *forced_reason)
{
    if (s_state != ST_RECORDING) return;
    s_state = ST_SAVING;
    render(true);                                    // shown while the file is closed

    rec_result_t r;
    esp_err_t e = recorder_stop(&r);
    ESP_LOGI(TAG, "saved %s: %lu bytes, %lu ms, dropped %lu, wav %lu Hz, mic %lu Hz (measured %lu), %s",
             r.path, (unsigned long)r.data_bytes, (unsigned long)r.duration_ms,
             (unsigned long)r.dropped_bytes, (unsigned long)r.sample_rate,
             (unsigned long)r.native_rate_hz, (unsigned long)r.measured_rate_hz, esp_err_to_name(e));

    if (forced_reason || e != ESP_OK) {
        notice(true, "RECORDING", forced_reason ? forced_reason : fault_text(e),
               r.data_bytes > 0 ? "Audio saved up to here" : "Nothing saved");
        // The file stays on the card; mark it so the idle retry picks it up later.
        if (r.data_bytes > 0) uploader_write_job(s_base, s_active.uid);
    } else if (r.data_bytes == 0) {
        notice(true, "RECORDING", "Too short", "Nothing saved");
    } else {
        uploader_write_job(s_base, s_active.uid);    // marks the file as "still to upload"
        if (wifi_mgr_has_creds() && start_upload(s_base, s_active.uid, false)) return;
        const unsigned s = (unsigned)(r.duration_ms / 1000);
        char dur[40];
        snprintf(dur, sizeof(dur), "%02u:%02u:%02u  upload later", s / 3600, (s / 60) % 60, s % 60);
        notice(false, "SAVED", s_active.name, dur);
    }
}

static void begin_recording(void)
{
    make_base_name(&s_active, s_base, sizeof(s_base));
    esp_err_t e = recorder_start(s_base);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "recorder_start: %s", esp_err_to_name(e));
        notice(true, "RECORDING", "Could not start", esp_err_to_name(e));
        return;
    }
    s_state = ST_RECORDING;
    s_rec_start_us = now_us();
    s_hint_until_us = 0;
    s_next_space_us = s_rec_start_us + 10 * 1000000LL;
    ESP_LOGI(TAG, "recording %s for %s (%s)", s_base, s_active.name, s_active.uid);
    render(true);
}

static void start_session(const char *uid)
{
    if (!config_complete(&s_cfg)) {
        notice(true, "NOT SET UP", "Hold button for", "admin mode");
        return;
    }
    const teacher_t *t = teachers_find_uid(uid);
    if (!t) {
        char l2[40];
        snprintf(l2, sizeof(l2), "UID %s", uid);
        notice(true, "UNKNOWN CARD", "Not registered", l2);
        return;
    }
    uint64_t freeb = 0;
    switch (sd_check(SD_MIN_START_BYTES, &freeb)) {
    case 1: notice(true, "SD CARD", "Card missing", "Insert the card"); return;
    case 2: notice(true, "SD CARD", "Cannot read card", "Check the card"); return;
    case 3: {
        char l2[40];
        snprintf(l2, sizeof(l2), "Only %lu MB free", (unsigned long)(freeb >> 20));
        notice(true, "SD CARD", "Card full", l2);
        return;
    }
    default: break;
    }
    if (!usb_mic_is_connected()) {
        notice(true, "MIC", "Not connected", "Plug in the USB mic");
        return;
    }
    s_active = *t;
    s_state = ST_COUNTDOWN;
    s_since_us = now_us();
    ESP_LOGI(TAG, "card %s -> %s (%s), starting countdown", uid, t->name, t->id);
    render(true);
}

/* ---------------------------------------------------------------- admin */
static void enter_admin(void)
{
    ESP_LOGI(TAG, "entering admin mode, BLE name %s", admin_ble_name());
    admin_ble_start();
    button_set_double_enabled(true);
    s_state = ST_ADMIN;
    render(true);
}

static void exit_admin(void)
{
    ESP_LOGI(TAG, "leaving admin mode");
    admin_ble_stop();
    button_set_double_enabled(false);
    config_load(&s_cfg);
    s_state = ST_NORMAL;
    render(true);
}

/* --------------------------------------------------------------- events */
static void on_tap(const char *uid)
{
    switch (s_state) {
    case ST_NORMAL:
        start_session(uid);
        break;
    case ST_RECORDING:
        if (strcmp(uid, s_active.uid) == 0) {
            if ((now_us() - s_rec_start_us) >= (int64_t)TAP_GUARD_MS * 1000) {
                ESP_LOGI(TAG, "same card tapped: stopping");
                stop_recording(NULL);
            }
        } else {
            ESP_LOGI(TAG, "other card %s tapped during recording", uid);
            s_hint_until_us = now_us() + (int64_t)HINT_MS * 1000;
        }
        break;
    case ST_UPLOADING:
        if (s_up_retry) {
            // An old file is being uploaded in the background: stop it (it stays pending) and start the lesson.
            ESP_LOGI(TAG, "card tapped during a background upload: cancelling it");
            strlcpy(s_deferred_uid, uid, sizeof(s_deferred_uid));
            uploader_cancel();
        } else {
            s_hint_until_us = now_us() + (int64_t)HINT_MS * 1000;
        }
        break;
    case ST_ADMIN:
        admin_ble_notify_tap(uid);
        break;
    default:
        break;
    }
}

static void handle_event(const app_event_t *ev)
{
    switch (ev->type) {
    case EVT_TAP:
        on_tap(ev->uid);
        break;
    case EVT_BTN_LONG:
        if (s_state == ST_NORMAL) enter_admin();
        break;
    case EVT_BTN_SINGLE:
        if (s_state == ST_RECORDING) {
            ESP_LOGI(TAG, "button: stopping");
            stop_recording(NULL);
        } else if (s_state == ST_COUNTDOWN) {
            ESP_LOGI(TAG, "button: countdown cancelled");
            s_state = ST_NORMAL;
            render(true);
        }
        break;
    case EVT_BTN_DOUBLE:
    case EVT_ADMIN_EXIT:
        if (s_state == ST_ADMIN) exit_admin();
        break;
    case EVT_CONFIG_CHANGED:
        config_load(&s_cfg);
        ESP_LOGI(TAG, "config changed: class %s section %s", s_cfg.class_name, s_cfg.section);
        break;
    }
}

static void tick_idle_work(int64_t now)
{
    // SD card missing or failed at boot: keep trying (a card may be inserted later).
    if (!sd_card_is_mounted()) {
        if (now >= s_next_sd_retry_us) {
            s_next_sd_retry_us = now + (int64_t)SD_RETRY_INTERVAL_MS * 1000;
            if (sd_card_mount_retry(1) == ESP_OK) {
                ESP_LOGI(TAG, "SD card is now available");
                recorder_recover_parts();
                render(true);
            }
        }
        return;
    }
    // Upload recordings that never made it (no Wi-Fi earlier, server error, power loss ...).
    if (now >= s_next_pending_us && wifi_mgr_connected() && time_is_valid()) {
        s_next_pending_us = now + (int64_t)UPLOAD_RETRY_INTERVAL_MS * 1000;
        char base[64], uid[24];
        if (uploader_find_pending(base, sizeof(base), uid, sizeof(uid))) {
            ESP_LOGI(TAG, "pending recording %s -> uploading in the background", base);
            start_upload(base, uid, true);
        }
    }
}

static void tick(void)
{
    const int64_t now = now_us();
    switch (s_state) {
    case ST_NORMAL:
        tick_idle_work(now);
        break;
    case ST_COUNTDOWN:
        if (now - s_since_us >= (int64_t)COUNTDOWN_SECONDS * 1000000) begin_recording();
        break;
    case ST_RECORDING:
        if (recorder_needs_stop()) {
            ESP_LOGW(TAG, "recorder reported a fault");
            stop_recording(NULL);
        } else if (now >= s_next_space_us) {
            s_next_space_us = now + 10 * 1000000LL;
            if (sd_check(SD_MIN_RUN_BYTES, NULL) != 0) {
                ESP_LOGW(TAG, "SD card low on space or unreadable");
                stop_recording("SD card full");
            }
        }
        break;
    case ST_UPLOADING:
        poll_upload();
        break;
    case ST_NOTICE:
        if (now >= s_notice_until_us) {
            s_state = ST_NORMAL;
            render(true);
        }
        break;
    case ST_ADMIN:
        if (admin_ble_idle_ms() > ADMIN_TIMEOUT_MS) {
            ESP_LOGI(TAG, "admin mode timed out");
            exit_admin();
        }
        break;
    default:
        break;
    }

    if (now >= s_next_heap_us) {
        s_next_heap_us = now + 30 * 1000000LL;
        ESP_LOGI(TAG, "heap: internal %u (min %u), psram %u | wifi %s rssi %d | nfc %d mic %d sd %d | teachers %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 wifi_mgr_connected() ? "up" : "down", wifi_mgr_rssi(),
                 nfc_ok(), usb_mic_is_connected(), sd_card_is_mounted(), (unsigned)teachers_count());
    }
    render(false);
}

void app_ctrl_run(uint32_t boot_count, bool have_display)
{
    s_boot = boot_count;
    s_display = have_display;
    config_load(&s_cfg);
    ESP_LOGI(TAG, "boot #%lu, class \"%s\" section \"%s\", ssid \"%s\", %u teacher(s)",
             (unsigned long)s_boot, s_cfg.class_name, s_cfg.section, s_cfg.ssid,
             (unsigned)teachers_count());
    s_state = ST_NORMAL;
    s_next_pending_us = now_us() + 30 * 1000000LL;     // give Wi-Fi and the clock time to come up first
    s_next_sd_retry_us = now_us() + (int64_t)SD_RETRY_INTERVAL_MS * 1000;
    render(true);

    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(100)) == pdTRUE) handle_event(&ev);
        tick();
    }
}
