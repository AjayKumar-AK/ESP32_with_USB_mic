#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char pass[65];
    char class_name[16];
    char section[8];
} app_config_t;

esp_err_t config_init(void);                    // nvs_flash_init (erases on version change)
void config_load(app_config_t *c);              // empty strings when nothing is saved
esp_err_t config_save(const app_config_t *c);
uint32_t config_next_boot_count(void);          // increments and returns the persistent boot counter

static inline bool config_complete(const app_config_t *c) { return c->class_name[0] && c->section[0]; }
