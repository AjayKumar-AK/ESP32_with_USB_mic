#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nvs.h"
#include "teachers.h"
#include "config_store.h"

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "OK" : "ERR"; }
static unsigned char g_blob[TEACHERS_MAX * 160]; static size_t g_len; static char g_str[4][80]; static unsigned g_u32;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len) { (void)h;(void)k; if (!g_len) return ESP_ERR_NOT_FOUND; if (out) memcpy(out, g_blob, g_len); *len = g_len; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { (void)h;(void)k; memcpy(g_blob, v, len); g_len = len; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h;(void)k; g_len = 0; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *l) { (void)h;(void)k;(void)o;(void)l; return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { (void)h;(void)k; *v = g_u32; return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { (void)h;(void)k; g_u32 = v; return ESP_OK; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

int main(void)
{
    char u[21];
    teachers_norm_uid("04:a1-b2 c3", u, sizeof(u));
    assert(strcmp(u, "04A1B2C3") == 0);

    teachers_init();
    assert(teachers_count() == 0);
    assert(teachers_add("04:a1:b2:c3", "T 001/x", "Priya Sharma", "abc123TOKEN") == ESP_OK);
    assert(teachers_add("04D4E5F6A7B890", "", "", "") == ESP_OK);
    assert(teachers_count() == 2);
    const teacher_t *t = teachers_find_uid("04a1b2c3");
    assert(t && strcmp(t->id, "T_001_x") == 0 && strcmp(t->name, "Priya Sharma") == 0);
    t = teachers_find_uid("04D4E5F6A7B890");
    assert(t && strcmp(t->id, "04D4E5F6A7B890") == 0 && strcmp(t->name, t->id) == 0);   // fallbacks
    assert(teachers_add("04A1B2C3", "T001", "Priya S", "") == ESP_OK && teachers_count() == 2);   // replace
    assert(strcmp(teachers_find_uid("04A1B2C3")->name, "Priya S") == 0);
    assert(strcmp(teachers_find_uid("04A1B2C3")->qr, "abc123TOKEN") == 0);          // empty qr keeps the old token
    assert(teachers_add("04A1B2C3", "T001", "Priya S", "new\"quote") == ESP_ERR_INVALID_ARG);   // unsafe token rejected
    assert(teachers_add("04A1B2C3", "T001", "Priya S", "newTOKEN9") == ESP_OK && strcmp(teachers_find_uid("04A1B2C3")->qr, "newTOKEN9") == 0);

    teachers_init();                                   // reload from the saved blob
    assert(teachers_count() == 2 && teachers_find_uid("04A1B2C3"));
    assert(teachers_remove("04A1B2C3") == ESP_OK && teachers_count() == 1);
    assert(teachers_remove("04A1B2C3") == ESP_ERR_NOT_FOUND);
    assert(teachers_add("zz", "x", "y", "") == ESP_ERR_INVALID_ARG);                // no hex digits
    for (int i = 0; i < TEACHERS_MAX; i++) { char s[16]; snprintf(s, sizeof s, "%08X", i + 1); if (teachers_add(s, "id", "n", "") != ESP_OK) break; }
    assert(teachers_count() == TEACHERS_MAX);
    assert(teachers_add("FFFFFFFF", "id", "n", "") == ESP_ERR_NO_MEM);
    assert(teachers_clear() == ESP_OK && teachers_count() == 0);
    printf("teachers: all checks passed (struct %zu bytes)\n", sizeof(teacher_t));
    return 0;
}
