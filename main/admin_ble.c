#include "admin_ble.h"
#include "app_config.h"
#include "app_events.h"
#include "ble_uuids.h"
#include "config_store.h"
#include "teachers.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "admin_ble";

#define LINE_MAX 400

typedef struct { char s[LINE_MAX]; } cmdline_t;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_cmd_handle, s_resp_handle;
static volatile bool s_active, s_synced, s_notify_on;
static uint16_t s_mtu = 23;
static char s_name[24] = "TCHR-REC-0000";
static QueueHandle_t s_cmdq;
static SemaphoreHandle_t s_tx_lock;
static volatile int64_t s_last_activity_us;

static char s_line[LINE_MAX];
static size_t s_linelen;
static uint8_t s_rx[512];

static void advertise(void);

/* ------------------------------------------------------------ sending */
static void send_text(const char *text)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify_on) return;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const size_t chunk = s_mtu > 3 ? (size_t)(s_mtu - 3) : 20;
    size_t len = strlen(text), off = 0;
    while (off < len) {
        const size_t n = (len - off) > chunk ? chunk : (len - off);
        for (int attempt = 0; attempt < 10; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(text + off, n);
            if (om && ble_gatts_notify_custom(s_conn, s_resp_handle, om) == 0) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        off += n;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    xSemaphoreGive(s_tx_lock);
}

static void send_json(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (s) {
        const size_t n = strlen(s);
        char *line = malloc(n + 2);
        if (line) {
            memcpy(line, s, n);
            line[n] = '\n';
            line[n + 1] = 0;
            send_text(line);
            free(line);
        }
        cJSON_free(s);
    }
    cJSON_Delete(obj);
}

static cJSON *reply_new(const char *cmd, bool ok, const char *err)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "cmd", cmd);
    cJSON_AddBoolToObject(r, "ok", ok);
    if (err) cJSON_AddStringToObject(r, "err", err);
    return r;
}

static void reply(const char *cmd, bool ok, const char *err) { send_json(reply_new(cmd, ok, err)); }

/* ----------------------------------------------------------- commands */
static const char *str_of(cJSON *root, const char *key)
{
    cJSON *it = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(it) && it->valuestring ? it->valuestring : NULL;
}

static void handle_command(cJSON *root)
{
    const char *cmd = str_of(root, "cmd");
    if (!cmd) { reply("?", false, "missing_cmd"); return; }
    ESP_LOGI(TAG, "command: %s", cmd);

    if (!strcmp(cmd, "hello") || !strcmp(cmd, "status")) {
        app_config_t cfg;
        config_load(&cfg);
        cJSON *r = reply_new(cmd, true, NULL);
        cJSON_AddStringToObject(r, "device", s_name);
        cJSON_AddStringToObject(r, "fw", FW_VERSION);
        cJSON_AddStringToObject(r, "ssid", cfg.ssid);
        cJSON_AddStringToObject(r, "class", cfg.class_name);
        cJSON_AddStringToObject(r, "section", cfg.section);
        cJSON_AddNumberToObject(r, "teachers", (double)teachers_count());
        cJSON_AddBoolToObject(r, "wifi_connected", wifi_mgr_connected());
        cJSON_AddNumberToObject(r, "rssi", wifi_mgr_rssi());
        struct tm tm;
        char ts[32] = "not set";
        if (time_get_local(&tm)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddStringToObject(r, "time", ts);
        send_json(r);
    } else if (!strcmp(cmd, "set_wifi")) {
        const char *ssid = str_of(root, "ssid"), *pass = str_of(root, "pass");
        if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32 || (pass && strlen(pass) > 63)) {
            reply(cmd, false, "bad_wifi");
            return;
        }
        app_config_t cfg;
        config_load(&cfg);
        strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
        strlcpy(cfg.pass, pass ? pass : "", sizeof(cfg.pass));
        if (config_save(&cfg) != ESP_OK) { reply(cmd, false, "save_failed"); return; }
        reply(cmd, true, NULL);
        wifi_mgr_apply(cfg.ssid, cfg.pass);
    } else if (!strcmp(cmd, "set_class")) {
        const char *cls = str_of(root, "class"), *sec = str_of(root, "section");
        if (!cls || !sec || !cls[0] || !sec[0] || strlen(cls) > 15 || strlen(sec) > 7) {
            reply(cmd, false, "bad_class");
            return;
        }
        app_config_t cfg;
        config_load(&cfg);
        strlcpy(cfg.class_name, cls, sizeof(cfg.class_name));
        strlcpy(cfg.section, sec, sizeof(cfg.section));
        if (config_save(&cfg) != ESP_OK) { reply(cmd, false, "save_failed"); return; }
        app_post(EVT_CONFIG_CHANGED);
        reply(cmd, true, NULL);
    } else if (!strcmp(cmd, "set_time")) {
        cJSON *e = cJSON_GetObjectItem(root, "epoch");
        if (!cJSON_IsNumber(e) || e->valuedouble < 1767225600.0) { reply(cmd, false, "bad_epoch"); return; }
        time_set_epoch((int64_t)e->valuedouble);
        struct tm tm;
        char ts[32] = "";
        if (time_get_local(&tm)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON *r = reply_new(cmd, true, NULL);
        cJSON_AddStringToObject(r, "time", ts);
        send_json(r);
    } else if (!strcmp(cmd, "add_teacher")) {
        const char *uid = str_of(root, "uid"), *id = str_of(root, "id"), *name = str_of(root, "name");
        const char *qr = str_of(root, "qr");
        if (!uid) { reply(cmd, false, "missing_uid"); return; }
        esp_err_t e = teachers_add(uid, id ? id : "", name ? name : "", qr ? qr : "");
        reply(cmd, e == ESP_OK, e == ESP_OK ? NULL
              : (e == ESP_ERR_NO_MEM ? "list_full" : (e == ESP_ERR_INVALID_ARG ? "bad_uid_or_token" : "save_failed")));
    } else if (!strcmp(cmd, "remove_teacher")) {
        const char *uid = str_of(root, "uid");
        esp_err_t e = uid ? teachers_remove(uid) : ESP_ERR_INVALID_ARG;
        reply(cmd, e == ESP_OK, e == ESP_OK ? NULL : (e == ESP_ERR_NOT_FOUND ? "not_found" : "failed"));
    } else if (!strcmp(cmd, "clear_teachers")) {
        reply(cmd, teachers_clear() == ESP_OK, NULL);
    } else if (!strcmp(cmd, "list_teachers")) {
        for (size_t i = 0; i < teachers_count(); i++) {
            const teacher_t *t = teachers_get(i);
            cJSON *m = cJSON_CreateObject();
            cJSON *o = cJSON_AddObjectToObject(m, "teacher");
            cJSON_AddStringToObject(o, "uid", t->uid);
            cJSON_AddStringToObject(o, "id", t->id);
            cJSON_AddStringToObject(o, "name", t->name);
            cJSON_AddBoolToObject(o, "qr", t->qr[0] != 0);     // the token itself is never sent back
            send_json(m);
        }
        cJSON *r = reply_new(cmd, true, NULL);
        cJSON_AddNumberToObject(r, "count", (double)teachers_count());
        send_json(r);
    } else if (!strcmp(cmd, "exit")) {
        reply(cmd, true, NULL);
        app_post(EVT_ADMIN_EXIT);
    } else if (!strcmp(cmd, "reboot")) {
        reply(cmd, true, NULL);
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    } else {
        reply(cmd, false, "unknown_cmd");
    }
}

static void admin_task(void *arg)
{
    cmdline_t line;
    for (;;) {
        if (xQueueReceive(s_cmdq, &line, portMAX_DELAY) != pdTRUE) continue;
        s_last_activity_us = esp_timer_get_time();
        cJSON *root = cJSON_Parse(line.s);
        if (!root) {
            ESP_LOGW(TAG, "bad JSON: %.60s", line.s);
            reply("?", false, "bad_json");
            continue;
        }
        handle_command(root);
        cJSON_Delete(root);
        s_last_activity_us = esp_timer_get_time();
    }
}

/* --------------------------------------------------------------- GATT */
static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr == s_cmd_handle) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(s_rx)) len = sizeof(s_rx);
        if (ble_hs_mbuf_to_flat(ctxt->om, s_rx, len, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
        for (uint16_t i = 0; i < len; i++) {
            const char c = (char)s_rx[i];
            if (c == '\n' || c == '\r') {
                if (s_linelen > 0) {
                    cmdline_t l;
                    memcpy(l.s, s_line, s_linelen);
                    l.s[s_linelen] = 0;
                    xQueueSend(s_cmdq, &l, 0);
                    s_linelen = 0;
                }
            } else if (s_linelen < LINE_MAX - 1) {
                s_line[s_linelen++] = c;
            } else {
                s_linelen = 0;   // overflow: drop the line
            }
        }
        s_last_activity_us = esp_timer_get_time();
    }
    return 0;
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_CMD.u,  .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE,  .val_handle = &s_cmd_handle },
            { .uuid = &UUID_RESP.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_resp_handle },
            { 0 }
        },
    },
    { 0 }
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_last_activity_us = esp_timer_get_time();
            ESP_LOGI(TAG, "PC connected");
        } else if (s_active) {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "PC disconnected (reason %d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify_on = false;
        s_linelen = 0;
        if (s_active) advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active && s_conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_resp_handle) {
            s_notify_on = ev->subscribe.cur_notify;
            s_last_activity_us = esp_timer_get_time();
            ESP_LOGI(TAG, "notifications %s", s_notify_on ? "on" : "off");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        ESP_LOGI(TAG, "MTU %u", s_mtu);
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!s_synced || !s_active) return;
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == 0) ESP_LOGI(TAG, "advertising as %s", s_name);
    else if (rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start failed: %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_synced = true;
    if (s_active) advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason %d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t admin_ble_init(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "TCHR-REC-%02X%02X", mac[4], mac[5]);

    s_cmdq = xQueueCreate(4, sizeof(cmdline_t));
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_cmdq || !s_tx_lock) return ESP_ERR_NO_MEM;

    if (nimble_port_init() != ESP_OK) return ESP_FAIL;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(k_svcs) != 0 || ble_gatts_add_svcs(k_svcs) != 0) return ESP_FAIL;
    ble_svc_gap_device_name_set(s_name);

    xTaskCreate(admin_task, "admin_cmd", 6144, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE ready (not advertising until admin mode), name %s", s_name);
    return ESP_OK;
}

const char *admin_ble_name(void) { return s_name; }

void admin_ble_start(void)
{
    s_active = true;
    s_last_activity_us = esp_timer_get_time();
    advertise();
}

void admin_ble_stop(void)
{
    s_active = false;
    ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "admin BLE stopped");
}

void admin_ble_notify_tap(const char *uid)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "tap", uid);
    send_json(m);
    s_last_activity_us = esp_timer_get_time();
}

uint32_t admin_ble_idle_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000);
}
