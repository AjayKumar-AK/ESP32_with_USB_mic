#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Uploads a finished recording the way the working Arduino test does it:
//   1. POST /device/toggle-session {"qrToken": ...}  -> recordingToken
//   2. POST /device/submit-audio   multipart: recordingToken + audio file
// Runs in its own task and streams the file from the SD card, so the UI can show progress.

typedef enum { UP_IDLE, UP_WAIT_WIFI, UP_WAIT_TIME, UP_SESSION, UP_UPLOAD, UP_SERVER, UP_DONE } up_phase_t;

typedef struct {
    bool active;
    up_phase_t phase;
    uint32_t done_bytes, total_bytes;   // file bytes sent / file size
    char status[28];                    // short text for the display
    char teacher[32];
} upload_progress_t;

typedef struct {
    bool ok;
    bool cancelled;
    char msg[40];
    char session[48];                   // uploadedSessionId from the server
    char base[64];
} upload_result_t;

void uploader_init(void);
bool uploader_submit(const char *base, const char *uid, bool is_retry);   // false if already busy
void uploader_cancel(void);
bool uploader_busy(void);
void uploader_get_progress(upload_progress_t *p);
bool uploader_take_result(upload_result_t *r);                            // true once per finished job

// A "<base>.job" file next to "<base>.wav" marks a recording that still has to be uploaded.
bool uploader_write_job(const char *base, const char *uid);
bool uploader_find_pending(char *base, size_t base_cap, char *uid, size_t uid_cap);
// Deletes the oldest files in /uploaded until at least want_free bytes are free.
void uploader_cleanup_uploaded(uint64_t want_free);
