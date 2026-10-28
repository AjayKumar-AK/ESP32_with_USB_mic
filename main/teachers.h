#pragma once
#include <stddef.h>
#include "esp_err.h"

#define TEACHERS_MAX 100

typedef struct {
    char uid[21];     // card UID, uppercase hex, e.g. "04A1B2C3"
    char id[16];      // teacher ID used in file names (letters, digits, - and _)
    char name[32];    // shown on the display
    char qr[64];      // the teacher's qrToken for the upload API (kept in flash, never shown or logged)
} teacher_t;

esp_err_t teachers_init(void);                           // loads the list from flash
size_t teachers_count(void);
const teacher_t *teachers_get(size_t i);
const teacher_t *teachers_find_uid(const char *uid);
// Adds or replaces by UID and saves. An empty qr keeps the token already stored for that card.
esp_err_t teachers_add(const char *uid, const char *id, const char *name, const char *qr);
esp_err_t teachers_remove(const char *uid);
esp_err_t teachers_clear(void);
void teachers_norm_uid(const char *in, char *out, size_t out_size);          // uppercase hex only
