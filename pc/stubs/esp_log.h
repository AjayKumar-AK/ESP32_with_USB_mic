#pragma once
#include <stdio.h>
#include <stdbool.h>
void esp_log_level_set(const char *t, int l);
#define ESP_LOG_DEBUG 4
#define ESP_LOGI(t, f, ...) printf("I %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) printf("W %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGE(t, f, ...) printf("E %s: " f "\n", t, ##__VA_ARGS__)
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t f);
