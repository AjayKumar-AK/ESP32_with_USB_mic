#pragma once
#include "esp_err.h"
typedef void *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ON_DATA, HTTP_EVENT_OTHER } esp_http_client_event_id_t;
typedef struct { esp_http_client_event_id_t event_id; void *data; int data_len; void *user_data; } esp_http_client_event_t;
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef struct {
    const char *url; esp_http_client_method_t method; int timeout_ms;
    esp_err_t (*event_handler)(esp_http_client_event_t *); void *user_data;
    int (*crt_bundle_attach)(void *); int keep_alive_enable;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int n);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len);
int esp_http_client_write(esp_http_client_handle_t c, const char *d, int n);
int esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *b, int n);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
