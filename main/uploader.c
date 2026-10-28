#include "uploader.h"
#include "app_config.h"
#include "sd_card.h"
#include "teachers.h"
#include "wifi_mgr.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "upload";

#define BOUNDARY "----ESP32Boundary7MA4YWxkTrZu0gW"
#define UPLOADED_DIR SD_MOUNT_POINT "/uploaded"

typedef struct {
    char base[64];
    char uid[24];
    bool retry;
} job_t;

static QueueHandle_t s_jobq;
static SemaphoreHandle_t s_lock;
static atomic_bool s_busy, s_cancel;
static upload_progress_t s_prog;
static upload_result_t s_res;
static bool s_res_ready;

/* ------------------------------------------------------------ shared state */
static void set_status(up_phase_t ph, const char *txt)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_prog.phase = ph;
    strlcpy(s_prog.status, txt, sizeof(s_prog.status));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", txt);
}

static void set_bytes(uint32_t done, uint32_t total)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_prog.done_bytes = done;
    s_prog.total_bytes = total;
    xSemaphoreGive(s_lock);
}

void uploader_get_progress(upload_progress_t *p)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *p = s_prog;
    xSemaphoreGive(s_lock);
}

bool uploader_take_result(upload_result_t *r)
{
    bool ready;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ready = s_res_ready;
    if (ready) { *r = s_res; s_res_ready = false; }
    xSemaphoreGive(s_lock);
    return ready;
}

bool uploader_busy(void) { return atomic_load(&s_busy); }
void uploader_cancel(void) { atomic_store(&s_cancel, true); }

/* -------------------------------------------------------------- HTTP helpers */
typedef struct { char *buf; size_t cap, len; } rx_t;

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data && e->data_len > 0) {
        rx_t *r = (rx_t *)e->user_data;
        size_t n = (size_t)e->data_len;
        if (r->len + n >= r->cap) n = r->cap - 1 - r->len;
        memcpy(r->buf + r->len, e->data, n);
        r->len += n;
        r->buf[r->len] = 0;
    }
    return ESP_OK;
}

static void fill_tls(esp_http_client_config_t *cfg)
{
#if UPLOAD_TLS_VERIFY
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
#else
    (void)cfg;      // no bundle and no certificate: needs CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY (set in sdkconfig.defaults)
#endif
}

// Returns the HTTP status, or -1 on a network error.
static int post_json(const char *url, const char *json, char *out, size_t cap)
{
    rx_t rx = { out, cap, 0 };
    out[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .event_handler = on_http_event,
        .user_data = &rx,
        .keep_alive_enable = false,
    };
    fill_tls(&cfg);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, (int)strlen(json));
    esp_err_t e = esp_http_client_perform(c);
    int status = (e == ESP_OK) ? esp_http_client_get_status_code(c) : -1;
    if (e != ESP_OK) ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(e));
    esp_http_client_cleanup(c);
    return status;
}

static bool wait_for(bool (*cond)(void), int timeout_ms)
{
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!cond()) {
        if (atomic_load(&s_cancel) || esp_timer_get_time() > end) return false;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return true;
}

/* ------------------------------------------------------------ the two API calls */
static bool start_session(const char *qr, char *token, size_t token_cap, char *err, size_t err_cap)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"qrToken\":\"%s\"}", qr);
    char resp[640];

    // toggle-session flips the session on and off: if the first call answers "stopped"
    // (a session was still open), a second call starts a fresh one.
    for (int pass = 0; pass < 2; pass++) {
        int code = post_json(API_BASE_URL "/device/toggle-session", body, resp, sizeof(resp));
        if (code < 200 || code >= 300) {
            if (code < 0) snprintf(err, err_cap, "No connection to server");
            else snprintf(err, err_cap, "Session refused (HTTP %d)", code);
            return false;
        }
        cJSON *j = cJSON_Parse(resp);
        const cJSON *act = j ? cJSON_GetObjectItem(j, "action") : NULL;
        const cJSON *tok = j ? cJSON_GetObjectItem(j, "recordingToken") : NULL;
        const cJSON *tn = j ? cJSON_GetObjectItem(j, "teacherName") : NULL;
        ESP_LOGI(TAG, "toggle-session: HTTP %d, action %s, teacher %s", code,
                 cJSON_IsString(act) ? act->valuestring : "?", cJSON_IsString(tn) ? tn->valuestring : "?");
        bool ok = cJSON_IsString(tok) && tok->valuestring && tok->valuestring[0];
        if (ok) strlcpy(token, tok->valuestring, token_cap);
        cJSON_Delete(j);
        if (ok) return true;
        if (pass == 0) ESP_LOGW(TAG, "no recordingToken in the reply, toggling again");
    }
    snprintf(err, err_cap, "Session did not start");
    return false;
}

static bool http_write_all(esp_http_client_handle_t c, const char *d, size_t n)
{
    size_t off = 0;
    int zero = 0;
    while (off < n) {
        int w = esp_http_client_write(c, d + off, (int)(n - off));
        if (w < 0) return false;
        if (w == 0) {
            if (++zero > 100) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        zero = 0;
        off += (size_t)w;
    }
    return true;
}

static bool upload_file(const char *path, const char *fname, const char *token,
                        char *session, size_t session_cap, char *err, size_t err_cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, err_cap, "Cannot open recording"); return false; }
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); snprintf(err, err_cap, "Recording is empty"); return false; }

    char pre[420];
    int pl = snprintf(pre, sizeof(pre),
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"recordingToken\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", token, fname);
    static const char epi[] = "\r\n--" BOUNDARY "--\r\n";
    const size_t el = sizeof(epi) - 1;
    const size_t total = (size_t)pl + (size_t)fsize + el;

    uint8_t *buf = heap_caps_malloc(UPLOAD_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) { fclose(f); snprintf(err, err_cap, "Out of memory"); return false; }

    esp_http_client_config_t cfg = {
        .url = API_BASE_URL "/device/submit-audio",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .keep_alive_enable = false,
    };
    fill_tls(&cfg);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (!c) { snprintf(err, err_cap, "HTTP init failed"); goto out; }
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);

    set_bytes(0, (uint32_t)fsize);
    set_status(UP_UPLOAD, "Uploading...");
    if (esp_http_client_open(c, (int)total) != ESP_OK) { snprintf(err, err_cap, "No connection to server"); goto out; }
    if (!http_write_all(c, pre, (size_t)pl)) { snprintf(err, err_cap, "Connection lost"); goto out; }

    {
        size_t sent = 0;
        uint32_t sd_ms = 0, net_ms = 0;
        const int64_t t0 = esp_timer_get_time();
        int64_t win = t0;
        size_t win_bytes = 0;
        while (sent < (size_t)fsize) {
            if (atomic_load(&s_cancel)) { snprintf(err, err_cap, "Cancelled"); goto out; }
            int64_t a = esp_timer_get_time();
            size_t n = fread(buf, 1, UPLOAD_CHUNK_BYTES, f);
            sd_ms += (uint32_t)((esp_timer_get_time() - a) / 1000);
            if (n == 0) { snprintf(err, err_cap, "SD read error"); goto out; }
            a = esp_timer_get_time();
            if (!http_write_all(c, (const char *)buf, n)) { snprintf(err, err_cap, "Connection lost"); goto out; }
            net_ms += (uint32_t)((esp_timer_get_time() - a) / 1000);
            sent += n;
            win_bytes += n;
            set_bytes((uint32_t)sent, (uint32_t)fsize);

            const int64_t now = esp_timer_get_time();
            if (now - win >= 2000000) {
                ESP_LOGI(TAG, "[%3d%%] %.1f/%.1f MB | now %.0f KB/s | avg %.0f KB/s",
                         (int)(sent * 100 / (size_t)fsize), sent / 1048576.0f, fsize / 1048576.0f,
                         (win_bytes / 1024.0f) / ((now - win) / 1e6f),
                         (sent / 1024.0f) / ((now - t0) / 1e6f));
                win = now;
                win_bytes = 0;
            }
        }
        ESP_LOGI(TAG, "sent %.2f MB in %.1f s (SD read %.1f s, network %.1f s)", fsize / 1048576.0f,
                 (esp_timer_get_time() - t0) / 1e6f, sd_ms / 1000.0f, net_ms / 1000.0f);
    }
    if (!http_write_all(c, epi, el)) { snprintf(err, err_cap, "Connection lost"); goto out; }

    set_status(UP_SERVER, "Server processing...");
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    char body[256] = {0};
    int got = 0, r;
    while (got < (int)sizeof(body) - 1 && (r = esp_http_client_read(c, body + got, sizeof(body) - 1 - got)) > 0) got += r;
    ESP_LOGI(TAG, "submit-audio: HTTP %d, %.120s", status, body);
    if (status == 200 || status == 201) {
        cJSON *j = cJSON_Parse(body);
        const cJSON *u = j ? cJSON_GetObjectItem(j, "uploadedSessionId") : NULL;
        strlcpy(session, cJSON_IsString(u) ? u->valuestring : "", session_cap);
        cJSON_Delete(j);
        ok = true;
    } else if (status <= 0) {
        snprintf(err, err_cap, "No reply from server");
    } else {
        snprintf(err, err_cap, "Upload refused (HTTP %d)", status);
    }

out:
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    heap_caps_free(buf);
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------- one job */
static bool wifi_ready(void) { return wifi_mgr_connected(); }
static bool clock_ready(void) { return time_is_valid(); }

static void finish(bool ok, bool cancelled, const char *msg, const char *session, const char *base)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_res, 0, sizeof(s_res));
    s_res.ok = ok;
    s_res.cancelled = cancelled;
    strlcpy(s_res.msg, msg ? msg : "", sizeof(s_res.msg));
    strlcpy(s_res.session, session ? session : "", sizeof(s_res.session));
    strlcpy(s_res.base, base, sizeof(s_res.base));
    s_res_ready = true;
    s_prog.active = false;
    s_prog.phase = UP_DONE;
    xSemaphoreGive(s_lock);
    atomic_store(&s_busy, false);
}

static void run_job(const job_t *j)
{
    char err[40] = "";
    char wav[96], job[96], fname[72];
    snprintf(wav, sizeof(wav), "%s/%s.wav", SD_MOUNT_POINT, j->base);
    snprintf(job, sizeof(job), "%s/%s.job", SD_MOUNT_POINT, j->base);
    snprintf(fname, sizeof(fname), "%s.wav", j->base);

    set_status(UP_WAIT_WIFI, "Waiting for Wi-Fi...");
    if (!wait_for(wifi_ready, UPLOAD_WIFI_WAIT_MS)) {
        if (atomic_load(&s_cancel)) { finish(false, true, "Cancelled", "", j->base); return; }
        finish(false, false, "No Wi-Fi", "", j->base);
        return;
    }
    set_status(UP_WAIT_TIME, "Syncing clock...");
    if (!wait_for(clock_ready, UPLOAD_TIME_WAIT_MS)) {
        if (atomic_load(&s_cancel)) { finish(false, true, "Cancelled", "", j->base); return; }
        finish(false, false, "Clock not synced", "", j->base);
        return;
    }

    const teacher_t *t = teachers_find_uid(j->uid);
    if (!t || !t->qr[0]) { finish(false, false, "No QR token for this card", "", j->base); return; }

    set_status(UP_SESSION, "Starting session...");
    char token[96];
    if (!start_session(t->qr, token, sizeof(token), err, sizeof(err))) {
        finish(false, atomic_load(&s_cancel), err, "", j->base);
        return;
    }

    char session[48] = "";
    bool ok = false;
    for (int attempt = 1; attempt <= UPLOAD_MAX_ATTEMPTS && !ok; attempt++) {
        if (attempt > 1) {
            char txt[28];
            snprintf(txt, sizeof(txt), "Retry %d/%d...", attempt, UPLOAD_MAX_ATTEMPTS);
            set_status(UP_UPLOAD, txt);
            for (int i = 0; i < 4 * attempt && !atomic_load(&s_cancel); i++) vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (atomic_load(&s_cancel)) break;
        ok = upload_file(wav, fname, token, session, sizeof(session), err, sizeof(err));
        if (!ok) ESP_LOGW(TAG, "attempt %d failed: %s", attempt, err);
    }
    memset(token, 0, sizeof(token));

    if (ok) {
        mkdir(UPLOADED_DIR, 0775);
        char dst[128];
        snprintf(dst, sizeof(dst), "%s/%s.wav", UPLOADED_DIR, j->base);
        if (rename(wav, dst) != 0) ESP_LOGW(TAG, "could not move %s (errno %d)", wav, errno);
        remove(job);
        uploader_cleanup_uploaded(SD_KEEP_FREE_BYTES);
        finish(true, false, "Uploaded", session, j->base);
    } else {
        finish(false, atomic_load(&s_cancel), err[0] ? err : "Upload failed", "", j->base);
    }
}

static void uploader_task(void *arg)
{
    job_t j;
    for (;;) {
        if (xQueueReceive(s_jobq, &j, portMAX_DELAY) == pdTRUE) run_job(&j);
    }
}

void uploader_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_jobq = xQueueCreate(1, sizeof(job_t));
    xTaskCreate(uploader_task, "uploader", 10240, NULL, 3, NULL);
    ESP_LOGI(TAG, "uploader ready (%s)", API_BASE_URL);
}

bool uploader_submit(const char *base, const char *uid, bool is_retry)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) return false;
    atomic_store(&s_cancel, false);

    job_t j;
    memset(&j, 0, sizeof(j));
    strlcpy(j.base, base, sizeof(j.base));
    strlcpy(j.uid, uid, sizeof(j.uid));
    j.retry = is_retry;

    const teacher_t *t = teachers_find_uid(uid);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_prog, 0, sizeof(s_prog));
    s_prog.active = true;
    s_prog.phase = UP_WAIT_WIFI;
    strlcpy(s_prog.status, "Starting...", sizeof(s_prog.status));
    strlcpy(s_prog.teacher, t ? t->name : "", sizeof(s_prog.teacher));
    s_res_ready = false;
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_jobq, &j, 0) != pdTRUE) { atomic_store(&s_busy, false); return false; }
    ESP_LOGI(TAG, "queued %s%s", base, is_retry ? " (retry of a pending file)" : "");
    return true;
}

/* -------------------------------------------------- pending files and cleanup */
bool uploader_write_job(const char *base, const char *uid)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.job", SD_MOUNT_POINT, base);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%s\n", uid);
    fclose(f);
    return true;
}

bool uploader_find_pending(char *base, size_t base_cap, char *uid, size_t uid_cap)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".job") != 0) continue;
        char stem[64];
        if (len - 4 >= sizeof(stem)) continue;
        memcpy(stem, e->d_name, len - 4);
        stem[len - 4] = 0;

        char wav[96], job[300];
        snprintf(wav, sizeof(wav), "%s/%s.wav", SD_MOUNT_POINT, stem);
        snprintf(job, sizeof(job), "%s/%s", SD_MOUNT_POINT, e->d_name);
        struct stat st;
        if (stat(wav, &st) != 0) { remove(job); continue; }      // recording is gone: drop the stale marker

        FILE *f = fopen(job, "r");
        if (!f) continue;
        char line[32] = "";
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) {
                strlcpy(base, stem, base_cap);
                strlcpy(uid, line, uid_cap);
                found = true;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

void uploader_cleanup_uploaded(uint64_t want_free)
{
    for (int round = 0; round < 50; round++) {
        uint64_t total = 0, freeb = 0;
        if (sd_card_free_bytes(&total, &freeb) != ESP_OK || freeb >= want_free) return;

        DIR *d = opendir(UPLOADED_DIR);
        if (!d) return;
        char oldest[300] = "";
        time_t oldest_t = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char p[300];
            snprintf(p, sizeof(p), "%s/%s", UPLOADED_DIR, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            if (!oldest[0] || st.st_mtime < oldest_t) { strlcpy(oldest, p, sizeof(oldest)); oldest_t = st.st_mtime; }
        }
        closedir(d);
        if (!oldest[0]) return;
        ESP_LOGW(TAG, "card nearly full: deleting oldest uploaded recording %s", oldest);
        remove(oldest);
    }
}
