#include "teachers.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "teachers";
#define NS  "teach"
#define KEY "list"

static teacher_t s_list[TEACHERS_MAX];
static size_t s_n;

void teachers_norm_uid(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; in && *in && o + 1 < out_size; in++) {
        char c = (char)toupper((unsigned char)*in);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) out[o++] = c;
    }
    out[o] = 0;
}

static void clean_id(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; in && *in && o + 1 < out_size; in++) {
        char c = *in;
        out[o++] = (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    }
    out[o] = 0;
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if (s_n == 0) {
        nvs_erase_key(h, KEY);
        e = ESP_OK;
    } else {
        e = nvs_set_blob(h, KEY, s_list, s_n * sizeof(teacher_t));
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    return e;
}

esp_err_t teachers_init(void)
{
    s_n = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   // nothing saved yet
    size_t len = 0;
    if (nvs_get_blob(h, KEY, NULL, &len) == ESP_OK && len > 0 &&
        len % sizeof(teacher_t) == 0 && len / sizeof(teacher_t) <= TEACHERS_MAX) {
        if (nvs_get_blob(h, KEY, s_list, &len) == ESP_OK) s_n = len / sizeof(teacher_t);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%u teacher(s) loaded", (unsigned)s_n);
    return ESP_OK;
}

size_t teachers_count(void) { return s_n; }
const teacher_t *teachers_get(size_t i) { return i < s_n ? &s_list[i] : NULL; }

const teacher_t *teachers_find_uid(const char *uid)
{
    char u[21];
    teachers_norm_uid(uid, u, sizeof(u));
    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_list[i].uid, u) == 0) return &s_list[i];
    }
    return NULL;
}

static bool qr_valid(const char *q)
{
    for (; *q; q++) {
        if (!(isalnum((unsigned char)*q) || *q == '-' || *q == '_')) return false;
    }
    return true;
}

esp_err_t teachers_add(const char *uid, const char *id, const char *name, const char *qr)
{
    teacher_t t;
    memset(&t, 0, sizeof(t));
    teachers_norm_uid(uid, t.uid, sizeof(t.uid));
    clean_id(id, t.id, sizeof(t.id));
    strlcpy(t.name, name ? name : "", sizeof(t.name));
    if (qr && !qr_valid(qr)) return ESP_ERR_INVALID_ARG;
    strlcpy(t.qr, qr ? qr : "", sizeof(t.qr));
    if (t.uid[0] == 0) return ESP_ERR_INVALID_ARG;
    if (t.id[0] == 0) strlcpy(t.id, t.uid, sizeof(t.id));
    if (t.name[0] == 0) strlcpy(t.name, t.id, sizeof(t.name));

    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_list[i].uid, t.uid) == 0) {
            if (t.qr[0] == 0) strlcpy(t.qr, s_list[i].qr, sizeof(t.qr));   // keep the old token
            s_list[i] = t;
            return save();
        }
    }
    if (s_n >= TEACHERS_MAX) return ESP_ERR_NO_MEM;
    s_list[s_n++] = t;
    return save();
}

esp_err_t teachers_remove(const char *uid)
{
    char u[21];
    teachers_norm_uid(uid, u, sizeof(u));
    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_list[i].uid, u) == 0) {
            memmove(&s_list[i], &s_list[i + 1], (s_n - i - 1) * sizeof(teacher_t));
            s_n--;
            return save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t teachers_clear(void)
{
    s_n = 0;
    return save();
}
