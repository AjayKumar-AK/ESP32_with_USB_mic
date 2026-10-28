#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define REC_PATH_MAX 128

typedef enum { REC_IDLE, REC_RECORDING, REC_FINALIZING } rec_state_t;

typedef struct {
    char     path[REC_PATH_MAX];   // final .wav path
    uint32_t sample_rate;          // rate written to the WAV header
    uint8_t  channels;
    uint8_t  bits;
    uint32_t data_bytes;           // PCM bytes actually on the card
    uint32_t dropped_bytes;        // lost to ring-buffer overflow
    uint32_t duration_ms;
    uint32_t native_rate_hz;       // rate negotiated with the mic
    uint32_t measured_rate_hz;     // rate the mic really delivered
    esp_err_t error;               // ESP_OK if the session ended cleanly
} rec_result_t;

esp_err_t recorder_init(void);

// base_name has no extension, e.g. "T04A1B2C3_20261013_093015".
// Writes SD_MOUNT_POINT/<base_name>.wav.part while recording.
esp_err_t recorder_start(const char *base_name);

// Blocks until the file is closed and renamed to .wav. Returns result.error.
esp_err_t recorder_stop(rec_result_t *result);

rec_state_t recorder_state(void);
// True when a recording hit a fatal fault (mic unplugged, SD write error, size limit).
// The caller should call recorder_stop() to save what was captured.
bool recorder_needs_stop(void);

// Finishes any *.part left by a power loss: fixes WAV sizes, renames to .wav.
void recorder_recover_parts(void);
