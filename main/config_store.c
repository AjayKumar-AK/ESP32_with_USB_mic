#include "config_store.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";
#define NS "cfg"

esp_err_t config_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    return e;
}

static void get(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t n = size;
    if (nvs_get_str(h, key, buf, &n) != ESP_OK) buf[0] = 0;
}

void config_load(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get(h, "ssid", c->ssid, sizeof(c->ssid));
    get(h, "pass", c->pass, sizeof(c->pass));
    get(h, "class", c->class_name, sizeof(c->class_name));
    get(h, "section", c->section, sizeof(c->section));
    nvs_close(h);
}

esp_err_t config_save(const app_config_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "ssid", c->ssid);
    nvs_set_str(h, "pass", c->pass);
    nvs_set_str(h, "class", c->class_name);
    nvs_set_str(h, "section", c->section);
    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    return e;
}

uint32_t config_next_boot_count(void)
{
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return 0;
    nvs_get_u32(h, "boots", &n);
    n++;
    nvs_set_u32(h, "boots", n);
    nvs_commit(h);
    nvs_close(h);
    return n;
}
