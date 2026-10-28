#include "wifi_mgr.h"
#include "app_config.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

static volatile bool s_connected;
static volatile bool s_have_creds;
static bool s_sntp_started;
static esp_timer_handle_t s_retry_timer;

#define TIME_VALID_AFTER 1767225600LL   // 2026-01-01 UTC: anything earlier means the clock was never set

static void retry_cb(void *arg)
{
    if (s_have_creds) esp_wifi_connect();
}

static void on_time_sync(struct timeval *tv)
{
    struct tm tm;
    time_t t = tv->tv_sec;
    localtime_r(&t, &tm);
    ESP_LOGI(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d IST",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_1);
    c.num_of_servers = 2;
    c.servers[1] = NTP_SERVER_2;
    c.sync_cb = on_time_sync;
    if (esp_netif_sntp_init(&c) == ESP_OK) s_sntp_started = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        if (s_have_creds) esp_timer_start_once(s_retry_timer, 3 * 1000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_connected = true;
        ESP_LOGI(TAG, "connected, ip " IPSTR ", rssi %d", IP2STR(&ev->ip_info.ip), wifi_mgr_rssi());
        start_sntp();
    }
}

static void set_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t c;
    memset(&c, 0, sizeof(c));
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    c.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &c);
}

esp_err_t wifi_mgr_init(const char *ssid, const char *pass)
{
    setenv("TZ", TZ_STRING, 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_have_creds = ssid && ssid[0];
    if (s_have_creds) {
        set_sta_config(ssid, pass ? pass : "");
        ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi credentials saved yet");
    }
    return esp_wifi_start();
}

void wifi_mgr_apply(const char *ssid, const char *pass)
{
    esp_timer_stop(s_retry_timer);
    s_have_creds = ssid && ssid[0];
    s_connected = false;
    esp_wifi_disconnect();
    if (s_have_creds) {
        set_sta_config(ssid, pass ? pass : "");
        ESP_LOGI(TAG, "new credentials, connecting to \"%s\"", ssid);
        esp_wifi_connect();
    }
}

bool wifi_mgr_has_creds(void) { return s_have_creds; }
bool wifi_mgr_connected(void) { return s_connected; }

int wifi_mgr_rssi(void)
{
    if (!s_connected) return 0;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

bool time_is_valid(void) { return (long long)time(NULL) > TIME_VALID_AFTER; }

bool time_get_local(struct tm *out)
{
    if (!time_is_valid()) return false;
    time_t t = time(NULL);
    localtime_r(&t, out);
    return true;
}

void time_set_epoch(int64_t epoch_utc)
{
    struct timeval tv = { .tv_sec = (time_t)epoch_utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}
