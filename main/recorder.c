#include "recorder.h"
#include "app_config.h"
#include "audio_proc.h"
#include "sd_card.h"
#include "usb_mic.h"
#include "wav.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "recorder";

#define EV_CAPTURE_DONE BIT0
#define EV_WRITER_DONE  BIT1

static RingbufHandle_t    s_rb;
static EventGroupHandle_t s_ev;
static volatile rec_state_t s_state = REC_IDLE;

static FILE *s_file;
static char  s_part_path[REC_PATH_MAX];
static char  s_final_path[REC_PATH_MAX];

static mic_format_t s_in;      // native mic format
static mic_format_t s_out;     // what is written to the WAV
static bool         s_processed;
static audio_proc_t s_proc;    // static: too big for a small task stack

static atomic_bool s_stop_req;
static atomic_bool s_capture_done;
static atomic_bool s_abort;
static atomic_uint_least32_t s_dropped;
static volatile esp_err_t s_err;

static uint32_t s_bytes_written;      // writer task only, read after it finishes
static uint64_t s_in_bytes;           // capture task only (native bytes from the mic)
static uint64_t s_in_first_bytes;
static int64_t  s_t_start_us, s_cap_first_us, s_cap_last_us;

static void ring_push(const uint8_t *data, size_t n)
{
    if (n && xRingbufferSend(s_rb, data, n, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, (uint_least32_t)n);
    }
}

// ---------------------------------------------------------------- capture
static void capture_task(void *arg)
{
    const size_t in_frame = (size_t)s_in.bytes_per_sample * s_in.channels;
    const size_t rd = (UAC_IO_CHUNK_BYTES / in_frame) * in_frame;   // frame-aligned reads
    uint8_t *buf = heap_caps_malloc(rd, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm = NULL;
    if (s_processed) {
        pcm = heap_caps_malloc(audio_proc_max_out(&s_proc, rd / in_frame) * sizeof(int16_t),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!buf || (s_processed && !pcm)) {
        s_err = ESP_ERR_NO_MEM;
        atomic_store(&s_abort, true);
    } else {
        int idle_reads = 0;
        while (!atomic_load(&s_stop_req) && !atomic_load(&s_abort)) {
            uint32_t got = 0;
            esp_err_t e = usb_mic_read(buf, (uint32_t)rd, &got, UAC_READ_TIMEOUT_MS);
            if (e == ESP_OK && got > 0) {
                idle_reads = 0;
                got -= got % in_frame;
                int64_t now = esp_timer_get_time();
                if (!s_cap_first_us) {
                    s_cap_first_us = now;
                    s_in_first_bytes = got;
                }
                s_cap_last_us = now;
                s_in_bytes += got;

                if (s_processed) {
                    size_t nout = audio_proc_run(&s_proc, (const int16_t *)buf,
                                                 got / in_frame, pcm);
                    ring_push((const uint8_t *)pcm, nout * sizeof(int16_t));
                } else {
                    ring_push(buf, got);
                }
            } else {
                idle_reads++;
            }

            if (usb_mic_has_fault()) {
                ESP_LOGE(TAG, "mic fault (unplugged or USB transfer error)");
                s_err = ESP_ERR_NOT_FOUND;
                atomic_store(&s_abort, true);
            } else if (idle_reads >= REC_NO_AUDIO_TIMEOUT_READS) {
                ESP_LOGE(TAG, "no audio from mic for %d ms",
                         REC_NO_AUDIO_TIMEOUT_READS * UAC_READ_TIMEOUT_MS);
                s_err = ESP_ERR_TIMEOUT;
                atomic_store(&s_abort, true);
            }
        }
    }
    if (buf) heap_caps_free(buf);
    if (pcm) heap_caps_free(pcm);
    atomic_store(&s_capture_done, true);
    xEventGroupSetBits(s_ev, EV_CAPTURE_DONE);
    vTaskDelete(NULL);
}

// ----------------------------------------------------------------- writer
static void writer_task(void *arg)
{
    // SD DMA wants internal memory; the ring buffer lives in PSRAM.
    uint8_t *buf = heap_caps_malloc(REC_WRITE_CHUNK_BYTES,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bool write_failed = (buf == NULL);
    if (!buf) {
        s_err = ESP_ERR_NO_MEM;
        atomic_store(&s_abort, true);
    }
    int64_t last_sync = esp_timer_get_time();

    for (;;) {
        // Sample "done" first: if it was already set, every send has completed,
        // so an empty receive really means the buffer is drained.
        bool done = atomic_load(&s_capture_done);
        size_t n = 0;
        uint8_t *item = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(100),
                                               REC_WRITE_CHUNK_BYTES);
        if (item) {
            if (!write_failed) {
                memcpy(buf, item, n);
                if ((uint64_t)s_bytes_written + n > REC_MAX_WAV_BYTES) {
                    ESP_LOGE(TAG, "WAV size limit reached");
                    s_err = ESP_ERR_INVALID_SIZE;
                    write_failed = true;
                    atomic_store(&s_abort, true);
                } else {
                    size_t w = fwrite(buf, 1, n, s_file);
                    s_bytes_written += (uint32_t)w;
                    if (w != n) {
                        ESP_LOGE(TAG, "SD write failed (errno %d)", errno);
                        s_err = ESP_FAIL;
                        write_failed = true;
                        atomic_store(&s_abort, true);
                    }
                }
            }
            vRingbufferReturnItem(s_rb, item);
        } else if (done) {
            break;
        }

        int64_t now = esp_timer_get_time();
        if (!write_failed && (now - last_sync) / 1000 >= REC_FSYNC_INTERVAL_MS) {
            fflush(s_file);
            fsync(fileno(s_file));
            last_sync = now;
        }
    }

    if (buf) heap_caps_free(buf);
    xEventGroupSetBits(s_ev, EV_WRITER_DONE);
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ API
esp_err_t recorder_init(void)
{
    s_ev = xEventGroupCreate();
    s_rb = xRingbufferCreateWithCaps(REC_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                     MALLOC_CAP_SPIRAM);
    if (!s_rb) {
        ESP_LOGW(TAG, "PSRAM ring buffer failed, using 64 KB internal");
        s_rb = xRingbufferCreate(64 * 1024, RINGBUF_TYPE_BYTEBUF);
    }
    if (!s_ev || !s_rb) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ring buffer ready");
    return ESP_OK;
}

rec_state_t recorder_state(void) { return s_state; }

bool recorder_needs_stop(void)
{
    return s_state == REC_RECORDING && atomic_load(&s_abort);
}

esp_err_t recorder_start(const char *base_name)
{
    if (s_state != REC_IDLE) return ESP_ERR_INVALID_STATE;
    if (!sd_card_is_mounted()) return ESP_ERR_NOT_FOUND;
    if (usb_mic_get_format(&s_in) != ESP_OK) return ESP_ERR_NOT_FOUND;

    // 16-bit mics: mono mix-down + DC block + FIR low-pass + decimate + gain.
    // Anything else (e.g. 24-bit) is stored untouched in its native format.
    s_processed = (s_in.bytes_per_sample == 2 && s_in.channels >= 1 && s_in.channels <= 2);
    if (s_processed) {
        audio_proc_init(&s_proc, s_in.sample_rate, s_in.channels,
                        OUTPUT_RATE_HZ, FIR_CUTOFF_HZ, DIGITAL_GAIN_X100);
        s_out.sample_rate = audio_proc_out_rate(&s_proc);
        s_out.channels = 1;
        s_out.bit_resolution = 16;
        s_out.bytes_per_sample = 2;
    } else {
        s_out = s_in;
        ESP_LOGW(TAG, "non-16-bit mic format: storing raw, unprocessed");
    }

    snprintf(s_part_path, sizeof(s_part_path), "%s/%s.wav.part", SD_MOUNT_POINT, base_name);
    snprintf(s_final_path, sizeof(s_final_path), "%s/%s.wav", SD_MOUNT_POINT, base_name);

    s_file = fopen(s_part_path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "cannot create %s (errno %d)", s_part_path, errno);
        return ESP_FAIL;
    }
    uint8_t hdr[WAV_HEADER_BYTES];
    wav_fill_header(hdr, s_out.sample_rate, s_out.channels,
                    (uint16_t)(s_out.bytes_per_sample * 8), 0);
    fwrite(hdr, 1, sizeof(hdr), s_file);
    fflush(s_file);
    fsync(fileno(s_file));

    esp_err_t err = usb_mic_start();
    if (err != ESP_OK) {
        fclose(s_file);
        s_file = NULL;
        remove(s_part_path);
        return err;
    }

    atomic_store(&s_stop_req, false);
    atomic_store(&s_capture_done, false);
    atomic_store(&s_abort, false);
    atomic_store(&s_dropped, 0);
    s_err = ESP_OK;
    s_bytes_written = 0;
    s_in_bytes = 0;
    s_in_first_bytes = 0;
    s_cap_first_us = s_cap_last_us = 0;
    s_t_start_us = esp_timer_get_time();
    xEventGroupClearBits(s_ev, EV_CAPTURE_DONE | EV_WRITER_DONE);

    s_state = REC_RECORDING;
    xTaskCreatePinnedToCore(writer_task, "rec_writer", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(capture_task, "rec_capture", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "recording %lu Hz %uch -> %lu Hz mono -> %s",
             (unsigned long)s_in.sample_rate, s_in.channels,
             (unsigned long)s_out.sample_rate, s_part_path);
    return ESP_OK;
}

esp_err_t recorder_stop(rec_result_t *result)
{
    if (s_state != REC_RECORDING) return ESP_ERR_INVALID_STATE;
    s_state = REC_FINALIZING;
    atomic_store(&s_stop_req, true);
    xEventGroupWaitBits(s_ev, EV_CAPTURE_DONE | EV_WRITER_DONE,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    usb_mic_stop();

    const uint32_t out_frame = (uint32_t)s_out.bytes_per_sample * s_out.channels;
    uint32_t data = s_bytes_written - (s_bytes_written % out_frame);

    uint8_t hdr[WAV_HEADER_BYTES];
    wav_fill_header(hdr, s_out.sample_rate, s_out.channels,
                    (uint16_t)(s_out.bytes_per_sample * 8), data);
    fflush(s_file);
    fseek(s_file, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), s_file);
    fflush(s_file);
    fsync(fileno(s_file));
    fclose(s_file);
    s_file = NULL;

    esp_err_t err = s_err;
    if (rename(s_part_path, s_final_path) != 0) {
        ESP_LOGE(TAG, "rename failed (errno %d), file stays as .part", errno);
        if (err == ESP_OK) err = ESP_FAIL;
    }

    // Rate the mic REALLY delivered (some mics ignore the requested rate).
    uint32_t measured = 0;
    if (s_cap_last_us > s_cap_first_us) {
        const uint32_t in_frame = (uint32_t)s_in.bytes_per_sample * s_in.channels;
        double secs = (double)(s_cap_last_us - s_cap_first_us) / 1e6;
        measured = (uint32_t)((double)(s_in_bytes - s_in_first_bytes) / in_frame / secs + 0.5);
        uint32_t diff = measured > s_in.sample_rate ? measured - s_in.sample_rate
                                                    : s_in.sample_rate - measured;
        if (diff * 100 > s_in.sample_rate * 3) {
            ESP_LOGW(TAG, "mic delivers ~%lu Hz but negotiated %lu Hz: WAV header rate may be wrong",
                     (unsigned long)measured, (unsigned long)s_in.sample_rate);
        }
    }

    if (result) {
        memset(result, 0, sizeof(*result));
        strlcpy(result->path, s_final_path, sizeof(result->path));
        result->sample_rate = s_out.sample_rate;
        result->channels = s_out.channels;
        result->bits = (uint8_t)(s_out.bytes_per_sample * 8);
        result->data_bytes = data;
        result->dropped_bytes = atomic_load(&s_dropped);
        result->duration_ms = (uint32_t)((esp_timer_get_time() - s_t_start_us) / 1000);
        result->native_rate_hz = s_in.sample_rate;
        result->measured_rate_hz = measured;
        result->error = err;
    }
    s_state = REC_IDLE;
    return err;
}

// --------------------------------------------------------------- recovery
void recorder_recover_parts(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".part") != 0) continue;

        char path[300], fin[300];
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, e->d_name);
        snprintf(fin, sizeof(fin), "%s/%.*s", SD_MOUNT_POINT, (int)(len - 5), e->d_name);

        FILE *f = fopen(path, "r+b");
        if (!f) continue;
        uint8_t h[WAV_HEADER_BYTES];
        if (fread(h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        uint32_t block = (uint32_t)h[32] | ((uint32_t)h[33] << 8);
        if (block == 0) block = 2;
        uint32_t data = sz > WAV_HEADER_BYTES ? (uint32_t)(sz - WAV_HEADER_BYTES) : 0;
        data -= data % block;
        bool ok = wav_patch_sizes(f, data);
        fsync(fileno(f));
        fclose(f);

        struct stat st;
        if (ok && stat(fin, &st) != 0 && rename(path, fin) == 0) {
            ESP_LOGW(TAG, "recovered %s (%lu bytes of audio)", fin, (unsigned long)data);
        } else {
            ESP_LOGW(TAG, "could not finalize %s, left as .part", path);
        }
    }
    closedir(d);
}
