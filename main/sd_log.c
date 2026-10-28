#include "sd_log.h"
#include "app_config.h"
#include "sd_card.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define SDLOG_BUF_BYTES       16384
#define SDLOG_MAX_FILE_BYTES  (2u * 1024 * 1024)   // one boot never writes more than this
#define SDLOG_KEEP_FILES      10                   // older boot logs are deleted
#define SDLOG_DIR             SD_MOUNT_POINT "/logs"

static StreamBufferHandle_t s_sb;
static vprintf_like_t s_orig;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_boot;

// Console output stays exactly as before; a copy goes to the buffer. Never blocks.
static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
    if (s_sb && !xPortInIsrContext()) {
        char line[200];
        int m = vsnprintf(line, sizeof(line), fmt, ap2);
        if (m > 0) {
            if (m >= (int)sizeof(line)) m = sizeof(line) - 1;
            portENTER_CRITICAL(&s_mux);
            xStreamBufferSend(s_sb, line, (size_t)m, 0);
            portEXIT_CRITICAL(&s_mux);
        }
    }
    va_end(ap2);
    return n;
}

static void writer_task(void *arg)
{
    FILE *f = NULL;
    uint32_t written = 0;
    bool capped = false;
    int64_t last_flush = 0;
    char buf[256];

    for (;;) {
        if (!sd_card_is_mounted()) {
            if (f) { fclose(f); f = NULL; }
            vTaskDelay(pdMS_TO_TICKS(500));          // keep early lines buffered until a card appears
            continue;
        }
        if (!f && !capped) {
            mkdir(SDLOG_DIR, 0775);
            char path[64];
            snprintf(path, sizeof(path), SDLOG_DIR "/boot_%05lu.txt", (unsigned long)s_boot);
            f = fopen(path, "a");
            if (!f) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            fprintf(f, "=== boot #%lu ===\n", (unsigned long)s_boot);
            if (s_boot > SDLOG_KEEP_FILES) {
                snprintf(path, sizeof(path), SDLOG_DIR "/boot_%05lu.txt", (unsigned long)(s_boot - SDLOG_KEEP_FILES));
                remove(path);
            }
        }

        size_t n = xStreamBufferReceive(s_sb, buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (n > 0 && f) {
            if (fwrite(buf, 1, n, f) != n) {
                fclose(f);
                f = NULL;                             // card trouble: reopen on the next pass
                continue;
            }
            written += (uint32_t)n;
            if (written > SDLOG_MAX_FILE_BYTES) {
                fputs("\n[log size limit reached, no further lines saved this boot]\n", f);
                fclose(f);
                f = NULL;
                capped = true;
            }
        }
        const int64_t now = xTaskGetTickCount() * (1000 / configTICK_RATE_HZ);
        if (f && now - last_flush >= 2000) {
            fflush(f);
            fsync(fileno(f));
            last_flush = now;
        }
    }
}

void sd_log_init(uint32_t boot_number)
{
    s_boot = boot_number;
    s_sb = xStreamBufferCreateWithCaps(SDLOG_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s_sb) s_sb = xStreamBufferCreate(4096, 1);
    if (!s_sb) return;
    xTaskCreate(writer_task, "sd_log", 4096, NULL, 1, NULL);
    s_orig = esp_log_set_vprintf(log_hook);
}
