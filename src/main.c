/*
 * ESP32-S3 Super Mini : USB mic (UAC host) -> WAV in LittleFS -> BLE (NimBLE)
 * ----------------------------------------------------------------------------
 * Standalone firmware, controlled directly from a PC via a simple Python
 * script -- no app involved.
 *
 * Boot workflow (fully automatic, no PC command needed):
 *   Board boots, inits USB host + BLE, advertises as "MIC-RECORDER" and
 *   starts mirroring every ESP_LOG line onto the LOG (notify) characteristic
 *   right away, so a PC connecting at any point sees the full log (a ring
 *   buffer replays anything logged before it subscribed).
 *   Waits BOOT_DELAY_MS (10s) after boot, then automatically records
 *   RECORD_SECONDS (15s) of audio from the USB mic to LittleFS.
 *   As soon as a central is connected and subscribed (indicate) to DATA,
 *   the saved WAV is uploaded automatically as acked BLE indications -- no
 *   CONTROL write is required for either step. The CONTROL characteristic
 *   is still present and can be written for manual overrides:
 *     write 0x02 to CONTROL -> records 5s from USB mic to LittleFS
 *     write 0x01 to CONTROL -> uploads the saved WAV as acked BLE indications
 *   After the upload finishes, an explicit "XFER_COMPLETE status=OK/FAIL"
 *   line is logged (and thus mirrored over BLE) so the PC side has an
 *   unambiguous, ESP-driven signal that the transfer is done -- it should
 *   not rely on a timeout or on counting bytes against the size header.
 *
 * Motor pulses (status):
 *   1 = recording started        2 = recording saved
 *   3 = BLE upload started       4 = BLE upload done
 *   5 = USB/mic init failed      6 = BLE init failed
 *   7 = recording failed
 *
 * sdkconfig requirements (menuconfig or sdkconfig.defaults):
 *   CONFIG_BT_ENABLED=y
 *   CONFIG_BT_NIMBLE_ENABLED=y
 *   (disable Bluedroid / classic BT — ESP32-S3 has no classic BT anyway)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"

#include "usb/usb_host.h"
#include "usb/uac_host.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ============================ USER SETTINGS ============================ */

#define MOTOR_IN1               3
#define MOTOR_IN2               12

#define PARTITION_LABEL         "littlefs"
#define BASE_PATH               "/littlefs"
#define RECORD_PATH             BASE_PATH "/recording.wav"

#define RECORD_SECONDS          15
#define OUTPUT_RATE_HZ          16000   /* decimate to this before storing (0 = keep native rate) */
#define RAW_TEST_MODE           0       /* 1 = ground-truth test: ~1-2 s of UNPROCESSED native-rate PCM
                                         *     (no decimation, no ADPCM) to judge the mic itself */
#define RAW_TEST_SECONDS        2
#define FIR_CUTOFF_HZ           6800.0f /* anti-alias low-pass cutoff before decimation */
#define DIGITAL_GAIN_X100       250     /* 100 = x1.0. 250 = +8 dB: your last take peaked at -10.4 dBFS, so this lands near -2.4 dBFS */
#define PREFERRED_SAMPLE_RATE   16000
#define BOOT_DELAY_MS           10000   /* wait after boot before auto-recording */

#define MIC_DETECT_TIMEOUT_MS   4000

#define UAC_RING_BUF_SIZE       (64 * 1024)
#define UAC_RING_THRESHOLD      (8 * 1024)
#define IO_CHUNK_BYTES          (4 * 1024)

#define BLE_DEVICE_NAME         "MIC-RECORDER"

/* Fixed UUIDs -- keep these matching between firmware and the PC script. */
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                      0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00);
static const ble_uuid128_t CONTROL_CHR_UUID =
    BLE_UUID128_INIT(0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                      0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x01);
static const ble_uuid128_t DATA_CHR_UUID =
    BLE_UUID128_INIT(0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                      0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x02);
static const ble_uuid128_t LOG_CHR_UUID =
    BLE_UUID128_INIT(0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                      0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x03);
/* --------------------------------------------------- */

/* ======================================================================= */

#define WAV_HEADER_SIZE         44
#define FS_MARGIN_BYTES         (16 * 1024)

/* Motor pulse meanings */
#define PULSE_RECORD_STARTED    1
#define PULSE_RECORD_SAVED      2
#define PULSE_UPLOAD_STARTED    3
#define PULSE_UPLOAD_DONE       4
#define PULSE_USB_INIT_FAILED   5
#define PULSE_BLE_INIT_FAILED   6
#define PULSE_RECORD_FAILED     7

static const char *TAG = "sen_recorder";

typedef struct { uint8_t addr; uint8_t iface_num; } mic_event_t;

typedef struct {
    uint8_t  channels;
    uint8_t  bit_resolution;
    uint8_t  bytes_per_sample;
    uint32_t sample_rate;
} mic_format_t;

static QueueHandle_t s_evt_q;
static volatile bool s_mic_gone;
static volatile bool s_xfer_error;

/* BLE state */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_data_chr_val_handle;
static uint16_t s_control_chr_val_handle;
static uint16_t s_log_chr_val_handle;
static volatile bool s_log_notify_enabled = false;
static volatile bool s_notify_enabled = false;
static volatile bool s_upload_requested = false;
static volatile bool s_record_requested = false;
static uint16_t s_att_mtu = 23; /* default until negotiated */
static volatile bool s_indicate_pending = false;
static volatile int s_indicate_status = -1;

/* Defined later; forward-declared so record_wav() (defined above it) can
 * send status strings too. */
static void ble_status(const char *msg);

/* Diagnostics reported over BLE by the heartbeat task */
static volatile bool s_fs_ok = false;
static volatile bool s_usb_ok = false;
static volatile bool s_mic_seen = false;
static const char *volatile s_rec_err = "none";
static const char *volatile s_stage = "boot";


/* ------------------------------------------------------------------ */
/* Motor status                                                        */
/* ------------------------------------------------------------------ */

static void motor_stop(void) { gpio_set_level(MOTOR_IN1, 0); gpio_set_level(MOTOR_IN2, 0); }
static void motor_run(void)  { gpio_set_level(MOTOR_IN1, 1); gpio_set_level(MOTOR_IN2, 0); }

static void motor_pulses(int count)
{
    for (int i = 0; i < count; i++) {
        motor_run();
        vTaskDelay(pdMS_TO_TICKS(200));
        motor_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------------ */
/* LittleFS                                                             */
/* ------------------------------------------------------------------ */

static bool littlefs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/* ------------------------------------------------------------------ */
/* WAV header                                                          */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wav_make_header(uint8_t h[WAV_HEADER_SIZE], uint32_t data_bytes,
                            uint32_t rate, uint16_t channels, uint16_t bits)
{
    const uint16_t block_align = (uint16_t)(channels * (bits / 8));
    memcpy(h + 0, "RIFF", 4);  put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);  put16(h + 20, 1);  put16(h + 22, channels);
    put32(h + 24, rate);  put32(h + 28, rate * block_align);
    put16(h + 32, block_align);  put16(h + 34, bits);
    memcpy(h + 36, "data", 4);  put32(h + 40, data_bytes);
}

/* ------------------------------------------------------------------ */
/* IMA ADPCM (4 bits/sample) encoder, block-based, WAV format tag 0x11  */
/* 16-bit PCM -> 4 bits = 4x smaller. Lets ~15 s of 16 kHz mono audio    */
/* fit in the ~150 KB of free internal RAM when there is no PSRAM, so    */
/* flash is never touched while the USB mic is streaming.               */
/* ------------------------------------------------------------------ */

#define ADPCM_BLOCK_BYTES        256
#define ADPCM_SAMPLES_PER_BLOCK  505          /* 1 in header + 504 coded (252 B) */
#define ADPCM_WAV_HEADER_SIZE    60

static const int16_t k_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t k_ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

typedef struct {
    uint8_t *base;        /* start of output buffer */
    uint8_t *wp;          /* write pointer */
    int32_t pred;         /* predictor */
    int32_t index;        /* step index */
    uint32_t blk_pos;     /* samples emitted in current block (0 = block not started) */
    bool low_nibble_next; /* next coded sample goes to the low nibble */
    uint32_t blocks;      /* completed blocks */
} adpcm_enc_t;

static uint8_t ima_encode_nibble(adpcm_enc_t *e, int16_t sample)
{
    int32_t step = k_ima_step[e->index];
    int32_t diff = (int32_t)sample - e->pred;
    uint8_t nib = 0;
    if (diff < 0) { nib = 8; diff = -diff; }
    int32_t vpdiff = step >> 3;
    if (diff >= step) { nib |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { nib |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { nib |= 1; vpdiff += step; }
    e->pred += (nib & 8) ? -vpdiff : vpdiff;
    if (e->pred > 32767) e->pred = 32767;
    if (e->pred < -32768) e->pred = -32768;
    e->index += k_ima_index[nib];
    if (e->index < 0) e->index = 0;
    if (e->index > 88) e->index = 88;
    return nib;
}

static void adpcm_init(adpcm_enc_t *e, uint8_t *buf)
{
    memset(e, 0, sizeof(*e));
    e->base = buf;
    e->wp = buf;
}

static void adpcm_feed(adpcm_enc_t *e, int16_t sample)
{
    if (e->blk_pos == 0) {
        /* block header: predictor (= first sample), step index, reserved */
        e->pred = sample;
        e->wp[0] = (uint8_t)(sample & 0xFF);
        e->wp[1] = (uint8_t)((sample >> 8) & 0xFF);
        e->wp[2] = (uint8_t)e->index;
        e->wp[3] = 0;
        e->wp += 4;
        e->blk_pos = 1;
        e->low_nibble_next = true;
        return;
    }
    uint8_t nib = ima_encode_nibble(e, sample);
    if (e->low_nibble_next) {
        *e->wp = nib;
        e->low_nibble_next = false;
    } else {
        *e->wp |= (uint8_t)(nib << 4);
        e->wp++;
        e->low_nibble_next = true;
    }
    if (++e->blk_pos == ADPCM_SAMPLES_PER_BLOCK) {
        e->blk_pos = 0;
        e->blocks++;
    }
}

/* Pad the last partial block with silence so every block is complete. */
static void adpcm_finish(adpcm_enc_t *e)
{
    while (e->blk_pos != 0) adpcm_feed(e, (int16_t)e->pred);
}

static void adpcm_make_header(uint8_t h[ADPCM_WAV_HEADER_SIZE], uint32_t data_bytes,
                              uint32_t rate, uint32_t total_samples)
{
    memcpy(h + 0, "RIFF", 4);  put32(h + 4, 52 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); put32(h + 16, 20);
    put16(h + 20, 0x0011);                 /* IMA ADPCM */
    put16(h + 22, 1);                      /* mono */
    put32(h + 24, rate);
    put32(h + 28, (uint32_t)(((uint64_t)rate * ADPCM_BLOCK_BYTES) / ADPCM_SAMPLES_PER_BLOCK));
    put16(h + 32, ADPCM_BLOCK_BYTES);      /* block align */
    put16(h + 34, 4);                      /* bits per sample */
    put16(h + 36, 2);                      /* cbSize */
    put16(h + 38, ADPCM_SAMPLES_PER_BLOCK);
    memcpy(h + 40, "fact", 4); put32(h + 44, 4); put32(h + 48, total_samples);
    memcpy(h + 52, "data", 4); put32(h + 56, data_bytes);
}

/* ------------------------------------------------------------------ */
/* Anti-alias decimator: DC blocker + windowed-sinc FIR low-pass,       */
/* then keep every Nth sample. Replaces the crude "average N samples"   */
/* filter, which only gives about -3.5 dB at the new Nyquist frequency   */
/* and therefore lets 8-24 kHz hiss/sibilance fold back into the speech  */
/* band as harsh, robotic-sounding aliasing.                            */
/* ------------------------------------------------------------------ */

#define FIR_TAPS 63

typedef struct {
    float h[FIR_TAPS];
    float x[FIR_TAPS];      /* circular input history */
    uint32_t pos;
    uint32_t phase;
    uint32_t decim;
    float dc_x1, dc_y1;     /* DC blocker state */
} fir_dec_t;

static void fir_init(fir_dec_t *d, uint32_t in_rate, uint32_t decim, float cutoff_hz)
{
    memset(d, 0, sizeof(*d));
    d->decim = decim;
    const float fc = cutoff_hz / (float)in_rate;          /* cycles per sample */
    const int mid = (FIR_TAPS - 1) / 2;
    float sum = 0.0f;
    for (int n = 0; n < FIR_TAPS; n++) {
        const int m = n - mid;
        const float sinc = (m == 0) ? 2.0f * fc
                                    : sinf(2.0f * 3.14159265f * fc * (float)m) / (3.14159265f * (float)m);
        const float w = 0.42f - 0.5f * cosf(2.0f * 3.14159265f * (float)n / (FIR_TAPS - 1))
                        + 0.08f * cosf(4.0f * 3.14159265f * (float)n / (FIR_TAPS - 1));
        d->h[n] = sinc * w;
        sum += d->h[n];
    }
    for (int n = 0; n < FIR_TAPS; n++) d->h[n] /= sum;    /* unity gain at DC */
}

/* Feed one input sample; returns true when an output sample is produced. */
static bool fir_push(fir_dec_t *d, float in, float *out)
{
    /* DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]  (R=0.998 -> ~15 Hz corner at 48 kHz) */
    const float dc = in - d->dc_x1 + 0.998f * d->dc_y1;
    d->dc_x1 = in;
    d->dc_y1 = dc;

    d->x[d->pos] = dc;
    d->pos = (d->pos + 1) % FIR_TAPS;
    if (++d->phase < d->decim) return false;
    d->phase = 0;

    float acc = 0.0f;
    uint32_t idx = d->pos;                                /* oldest sample */
    for (int k = 0; k < FIR_TAPS; k++) {
        acc += d->h[k] * d->x[idx];
        idx = (idx + 1) % FIR_TAPS;
    }
    *out = acc;
    return true;
}

static fir_dec_t s_fir;   /* static: too big for the small main-task stack */


/* ------------------------------------------------------------------ */
/* Format selection                                                     */
/* ------------------------------------------------------------------ */

static uint32_t abs_diff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static bool pick_format(uac_host_device_handle_t h, uint8_t alt_count, mic_format_t *best)
{
    bool found = false;
    int32_t best_score = 0;

    for (uint8_t alt = 1; alt <= alt_count; alt++) {
        uac_host_dev_alt_param_t p;
        memset(&p, 0, sizeof(p));
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK || p.channels == 0) {
            continue;
        }

        uint32_t rate = 0;
        if (p.sample_freq_type > 0) {
            size_t n = sizeof(p.sample_freq) / sizeof(p.sample_freq[0]);
            if (p.sample_freq_type < n) n = p.sample_freq_type;
            for (size_t i = 0; i < n; i++) {
                uint32_t f = p.sample_freq[i];
                if (f != 0 && (rate == 0 ||
                    abs_diff(f, PREFERRED_SAMPLE_RATE) < abs_diff(rate, PREFERRED_SAMPLE_RATE))) {
                    rate = f;
                }
            }
        } else {
            uint32_t lo = p.sample_freq_lower, hi = p.sample_freq_upper;
            if (lo == 0) lo = hi;
            if (hi == 0) hi = lo;
            rate = PREFERRED_SAMPLE_RATE;
            if (rate < lo) rate = lo;
            if (rate > hi) rate = hi;
        }
        if (rate == 0) continue;

        const uint8_t bps = p.subframe_size ? p.subframe_size
                                            : (uint8_t)((p.bit_resolution + 7) / 8);
        ESP_LOGI(TAG, "  alt %u: %u ch, %u-bit (%u B/sample), %lu Hz",
                 alt, p.channels, p.bit_resolution, bps, (unsigned long)rate);
        {
            char alt_msg[140];
            snprintf(alt_msg, sizeof(alt_msg),
                     "UAC_ALT %u ch=%u bits=%u sub=%u type=%u rate=%lu range=%lu-%lu",
                     alt, p.channels, p.bit_resolution, p.subframe_size, p.sample_freq_type,
                     (unsigned long)rate, (unsigned long)p.sample_freq_lower,
                     (unsigned long)p.sample_freq_upper);
            ble_status(alt_msg);
        }

        int32_t score = 0;
        if (p.bit_resolution == 16) score += 100000;
        if (p.channels == 1)        score += 10000;
        else if (p.channels == 2)   score += 5000;
        score -= (int32_t)(abs_diff(rate, PREFERRED_SAMPLE_RATE) / 100);

        if (!found || score > best_score) {
            found = true;
            best_score = score;
            best->channels = p.channels;
            best->bit_resolution = p.bit_resolution;
            best->bytes_per_sample = bps;
            best->sample_rate = rate;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* UAC callbacks                                                        */
/* ------------------------------------------------------------------ */

static void uac_driver_cb(uint8_t addr, uint8_t iface_num,
                          const uac_host_driver_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        mic_event_t e = { .addr = addr, .iface_num = iface_num };
        xQueueSend(s_evt_q, &e, 0);
    }
}

static void uac_device_cb(uac_host_device_handle_t handle,
                          const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED)      s_mic_gone = true;
    else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) s_xfer_error = true;
}

static void usb_host_task(void *arg)
{
    while (true) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB host event error: %s", esp_err_to_name(err));
        }
    }
}

static bool usb_mic_init(void)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&host_cfg) != ESP_OK) {
        return false;
    }
    xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 6, NULL);

    const uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority = 6,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = uac_driver_cb,
        .callback_arg = NULL,
    };
    return uac_host_install(&uac_cfg) == ESP_OK;
}

/* ------------------------------------------------------------------ */
/* RECORD                                                               */
/* ------------------------------------------------------------------ */

/* No-PSRAM mode: the capture loop only pushes decimated audio into a small
 * RAM FIFO (never touches flash); this separate task drains the FIFO to
 * LittleFS. If flash stalls, the FIFO absorbs it; if it overflows we count
 * the dropped bytes instead of ever blocking the USB read loop. */
typedef struct {
    StreamBufferHandle_t sb;
    FILE *f;
    uint32_t max_bytes;
    volatile uint32_t written;
    volatile bool stop;
    volatile bool failed;
    SemaphoreHandle_t done;
} flash_writer_t;

static void flash_writer_task(void *arg)
{
    flash_writer_t *w = (flash_writer_t *)arg;
    uint8_t *buf = malloc(4096);
    if (buf == NULL) w->failed = true;
    while (buf != NULL) {
        size_t n = xStreamBufferReceive(w->sb, buf, 4096, pdMS_TO_TICKS(50));
        if (n > 0) {
            if (!w->failed) {
                if (w->written + n > w->max_bytes || fwrite(buf, 1, n, w->f) != n) {
                    w->failed = true;
                } else {
                    w->written += (uint32_t)n;
                }
            }
        } else if (w->stop && xStreamBufferIsEmpty(w->sb)) {
            break;
        }
    }
    free(buf);
    xSemaphoreGive(w->done);
    vTaskDelete(NULL);
}

static bool record_wav(const mic_event_t *ev)
{
    uac_host_device_handle_t h = NULL;
    uac_host_dev_info_t info;
    mic_format_t fmt;
    FILE *f = NULL;
    uint8_t *chunk = NULL;
    uint8_t *rec = NULL;   /* whole capture lives in PSRAM; flash is written AFTER */
    uint32_t ram_bytes = 0;
    uint32_t decim = 1;         /* >1 only in the no-PSRAM fallback */
    uint32_t hdr_rate = 0;      /* sample rate written into the WAV header */
    int rec_seconds = RECORD_SECONDS;
    bool stream_mode = false;   /* no PSRAM: FIFO + writer task instead of a RAM buffer */
    uint8_t *outbuf = NULL;
    uint32_t fifo_dropped = 0;
    flash_writer_t fw;
    memset(&fw, 0, sizeof(fw));
    bool streaming = false;
    bool ok = false;
    uint32_t written = 0;
    uint8_t hdr[ADPCM_WAV_HEADER_SIZE];
    bool adpcm_mode = false;    /* no PSRAM: IMA ADPCM into internal RAM */
    adpcm_enc_t enc;
    uint32_t hdr_len = WAV_HEADER_SIZE;
    memset(&enc, 0, sizeof(enc));

    s_mic_gone = false;
    s_xfer_error = false;
    s_rec_err = "none";
    remove(RECORD_PATH); /* never let a failed record leave an old file to be uploaded */

    const uint32_t ring_bytes = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
                                ? UAC_RING_BUF_SIZE : (32 * 1024);
    const uac_host_device_config_t dev_cfg = {
        .addr = ev->addr,
        .iface_num = ev->iface_num,
        .buffer_size = ring_bytes,
        .buffer_threshold = UAC_RING_THRESHOLD,
        .callback = uac_device_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_device_open(&dev_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open: %s", esp_err_to_name(err));
        s_rec_err = "uac_open";
        return false;
    }

    memset(&info, 0, sizeof(info));
    if (uac_host_get_device_info(h, &info) == ESP_OK) {
        ESP_LOGI(TAG, "VID=0x%04X PID=0x%04X, %u alt setting(s)", info.VID, info.PID, info.iface_alt_num);
        char info_msg[120];
        snprintf(info_msg, sizeof(info_msg), "UAC_INFO VID=0x%04X PID=0x%04X iface=%u alts=%u",
                 info.VID, info.PID, info.iface_num, info.iface_alt_num);
        ble_status(info_msg);
    }

    memset(&fmt, 0, sizeof(fmt));
    if (!pick_format(h, info.iface_alt_num, &fmt)) {
        ESP_LOGE(TAG, "No usable audio format");
        s_rec_err = "no_format";
        goto done;
    }
    ESP_LOGI(TAG, "Using %u ch, %u-bit, %lu Hz", fmt.channels, fmt.bit_resolution,
             (unsigned long)fmt.sample_rate);
    {
        char fmt_msg[100];
        snprintf(fmt_msg, sizeof(fmt_msg), "FORMAT channels=%u bits=%u bytes=%u rate=%luHz",
                 fmt.channels, fmt.bit_resolution, fmt.bytes_per_sample,
                 (unsigned long)fmt.sample_rate);
        ble_status(fmt_msg);
    }

    const uint32_t frame_bytes = (uint32_t)fmt.channels * fmt.bytes_per_sample;
    /* Frame-align the read chunk size so we never split a sample frame
     * across two reads (that alone can cause crackle/robotic artifacts). */
    const uint32_t io_chunk_frames = IO_CHUNK_BYTES / frame_bytes;
    const uint32_t io_chunk_bytes_aligned = io_chunk_frames * frame_bytes;

    /* Generous upper bound just to size the file / check flash space --
     * NOT used to decide when to stop. We stop by wall-clock time instead,
     * because the mic's true native rate may not match what it negotiated. */
    uint32_t max_bytes = fmt.sample_rate * frame_bytes * (RECORD_SECONDS + 2);

    remove(RECORD_PATH);
    size_t fs_total = 0, fs_used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &fs_total, &fs_used) == ESP_OK) {
        size_t free_bytes = fs_total > fs_used ? fs_total - fs_used : 0;
        size_t usable = free_bytes > (FS_MARGIN_BYTES + WAV_HEADER_SIZE)
                        ? free_bytes - FS_MARGIN_BYTES - WAV_HEADER_SIZE : 0;
        if (usable < max_bytes) {
            ESP_LOGW(TAG, "Flash has room for %u B, wanted %lu B: capping",
                     (unsigned)usable, (unsigned long)max_bytes);
            max_bytes = (uint32_t)(usable - (usable % frame_bytes));
        }
    }
    if (max_bytes == 0) {
        ESP_LOGE(TAG, "No free space in LittleFS");
        s_rec_err = "no_flash_space";
        goto done;
    }

    f = fopen(RECORD_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot create %s", RECORD_PATH);
        s_rec_err = "fopen";
        goto done;
    }
    const uint16_t bits = (uint16_t)(fmt.bytes_per_sample * 8);
    wav_make_header(hdr, 0, fmt.sample_rate, fmt.channels, bits); /* placeholder, fixed up below */
    if (fwrite(hdr, 1, WAV_HEADER_SIZE, f) != WAV_HEADER_SIZE) {
        ESP_LOGE(TAG, "Header write failed");
        s_rec_err = "hdr_write";
        goto done;
    }

    chunk = malloc(io_chunk_bytes_aligned);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        s_rec_err = "chunk_malloc";
        goto done;
    }
    /* Capture into RAM first. Flash (LittleFS) writes stall for tens of ms
     * (block erase), which used to block the USB read loop and overflow the
     * driver's ring buffer -> dropped audio. RAM writes never stall. */
    /* Decimate (average N frames -> 1) down to ~OUTPUT_RATE_HZ before storing.
     * Averaging is a cheap low-pass so it doesn't alias, and it cuts RAM,
     * flash and BLE transfer time 3x for a 48 kHz mic. Speech is fine at 16 kHz. */
    hdr_rate = fmt.sample_rate;
    if (OUTPUT_RATE_HZ > 0 && fmt.bytes_per_sample == 2 && fmt.channels <= 2 &&
        fmt.sample_rate > OUTPUT_RATE_HZ) {
        decim = (fmt.sample_rate + OUTPUT_RATE_HZ / 2) / OUTPUT_RATE_HZ;
        if (decim < 1) decim = 1;
        hdr_rate = fmt.sample_rate / decim;
    }
    if (RAW_TEST_MODE) { /* ground truth: keep native rate, no processing */
        decim = 1;
        hdr_rate = fmt.sample_rate;
    }
    const uint32_t sec_bytes = hdr_rate * frame_bytes;
    ram_bytes = sec_bytes * (RECORD_SECONDS + 1);
    ram_bytes -= ram_bytes % frame_bytes;
    rec = heap_caps_malloc(ram_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rec == NULL && RAW_TEST_MODE) {
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t usable = big > 32 * 1024 ? big - 32 * 1024 : 0;
        int secs = (int)(usable / sec_bytes) - 1;
        if (secs > RAW_TEST_SECONDS) secs = RAW_TEST_SECONDS;
        if (secs < 1) {
            s_rec_err = "ram_malloc";
            goto done;
        }
        rec_seconds = secs;
        ram_bytes = sec_bytes * (rec_seconds + 1);
        ram_bytes -= ram_bytes % frame_bytes;
        rec = heap_caps_malloc(ram_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (rec == NULL) {
            s_rec_err = "ram_malloc";
            goto done;
        }
        char m[80];
        snprintf(m, sizeof(m), "RAW_TEST seconds=%d at %luHz (no decimation, no ADPCM)",
                 rec_seconds, (unsigned long)hdr_rate);
        ble_status(m);
    } else if (rec == NULL) {
        if (fmt.bytes_per_sample == 2 && fmt.channels <= 2) {
            /* No PSRAM: compress to IMA ADPCM (4x smaller) and keep the whole
             * clip in internal RAM, so flash is never written while USB streams. */
            adpcm_mode = true;
            size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            size_t usable = big > 32 * 1024 ? big - 32 * 1024 : 0;
            const uint32_t adpcm_bps = (uint32_t)(((uint64_t)hdr_rate * ADPCM_BLOCK_BYTES) /
                                                  ADPCM_SAMPLES_PER_BLOCK);
            int secs = (int)(usable / adpcm_bps) - 1;
            if (secs > RECORD_SECONDS) secs = RECORD_SECONDS;
            if (secs < 1) {
                s_rec_err = "ram_malloc";
                goto done;
            }
            rec_seconds = secs;
            ram_bytes = adpcm_bps * (rec_seconds + 1) + 3 * ADPCM_BLOCK_BYTES;
            ram_bytes -= ram_bytes % ADPCM_BLOCK_BYTES;
            rec = heap_caps_malloc(ram_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (rec == NULL) {
                s_rec_err = "ram_malloc";
                goto done;
            }
            adpcm_init(&enc, rec);
            fir_init(&s_fir, fmt.sample_rate, decim, FIR_CUTOFF_HZ);
            /* ADPCM header is longer than the 44-byte placeholder already written */
            hdr_len = ADPCM_WAV_HEADER_SIZE;
            adpcm_make_header(hdr, 0, hdr_rate, 0);
            if (fseek(f, 0, SEEK_SET) != 0 || fwrite(hdr, 1, hdr_len, f) != hdr_len) {
                s_rec_err = "hdr_write";
                goto done;
            }
            char m[80];
            snprintf(m, sizeof(m), "NO_PSRAM ADPCM capture_seconds=%d at %luHz",
                     rec_seconds, (unsigned long)hdr_rate);
            ble_status(m);
        } else {
            /* Unusual sample format: stream to flash through a small RAM FIFO. */
            stream_mode = true;
            ble_status("NO_PSRAM streaming to flash via FIFO");
            outbuf = malloc(io_chunk_bytes_aligned);
            if (outbuf == NULL) {
                s_rec_err = "outbuf_malloc";
                goto done;
            }
        }
    } else {
        char m[64];
        snprintf(m, sizeof(m), "PSRAM_CAPTURE seconds=%d at %luHz",
                 rec_seconds, (unsigned long)hdr_rate);
        ble_status(m);
    }

    const uac_host_stream_config_t stream_cfg = {
        .channels = fmt.channels,
        .bit_resolution = fmt.bit_resolution,
        .sample_freq = fmt.sample_rate,
        .flags = 0,
    };
    {
        char start_msg[140];
        snprintf(start_msg, sizeof(start_msg),
                 "UAC_START_REQ ch=%u bits=%u rate=%luHz ring=%lu threshold=%lu read=%lu",
                 stream_cfg.channels, stream_cfg.bit_resolution,
                 (unsigned long)stream_cfg.sample_freq, (unsigned long)ring_bytes,
                 (unsigned long)UAC_RING_THRESHOLD, (unsigned long)io_chunk_bytes_aligned);
        ble_status(start_msg);
    }
    err = uac_host_device_start(h, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_start: %s", esp_err_to_name(err));
        s_rec_err = "uac_start";
        goto done;
    }
    streaming = true;
    s_stage = "recording";

    ESP_LOGI(TAG, "RECORDING for %d real seconds (frame_bytes=%lu)",
             rec_seconds, (unsigned long)frame_bytes);

    /* Capture by WALL-CLOCK TIME, not by a nominal byte target. The mic's
     * negotiated rate (fmt.sample_rate) may not be what it actually streams
     * at -- some USB mic dongles ignore the requested rate entirely. Timing
     * the capture and measuring the true rate afterward is self-correcting
     * regardless of what the descriptor claims. */
    const int64_t capture_start_us = esp_timer_get_time();
    const int64_t capture_target_us = (int64_t)rec_seconds * 1000000LL;
    const int64_t hard_limit_us = (int64_t)(rec_seconds + 5) * 1000000LL;
    int64_t first_byte_us = 0;
    int64_t last_byte_us = 0;
    int32_t acc[2] = {0, 0};
    uint32_t acc_cnt = 0;
    uint32_t total_in = 0, read_count = 0;

    if (stream_mode) {
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t fifo_bytes = 48 * 1024;
        if (big < fifo_bytes + 32 * 1024) fifo_bytes = big > 48 * 1024 ? big - 32 * 1024 : 8 * 1024;
        fw.sb = xStreamBufferCreate(fifo_bytes, 1);
        fw.done = xSemaphoreCreateBinary();
        fw.f = f;
        fw.max_bytes = max_bytes;
        if (fw.sb == NULL || fw.done == NULL ||
            xTaskCreate(flash_writer_task, "flash_wr", 6144, &fw, 2, NULL) != pdPASS) {
            s_rec_err = "writer_create";
            if (fw.sb) vStreamBufferDelete(fw.sb);
            if (fw.done) vSemaphoreDelete(fw.done);
            goto done;
        }
    }

    while (!s_mic_gone && !s_xfer_error) {
        int64_t now = esp_timer_get_time();
        if ((now - capture_start_us) > hard_limit_us) {
            ESP_LOGW(TAG, "Hard time limit hit");
            break;
        }
        if (first_byte_us != 0 && (now - first_byte_us) >= capture_target_us) {
            break; /* collected RECORD_SECONDS of real time since first audio arrived */
        }
        if (stream_mode && fw.failed) {
            ESP_LOGE(TAG, "Flash writer failed");
            break;
        }
        if (adpcm_mode && (uint32_t)(enc.wp - rec) + 3 * ADPCM_BLOCK_BYTES > ram_bytes) {
            ESP_LOGW(TAG, "Hit ADPCM RAM cap before time limit");
            break;
        }
        if (!stream_mode && !adpcm_mode && written + io_chunk_bytes_aligned > ram_bytes) {
            ESP_LOGW(TAG, "Hit RAM capture cap before time limit");
            break;
        }

        uint32_t got = 0;
        const int64_t read_start_us = esp_timer_get_time();
        err = uac_host_device_read(h, chunk, io_chunk_bytes_aligned, &got, pdMS_TO_TICKS(500));
        const int64_t read_end_us = esp_timer_get_time();
        if (err == ESP_OK && got > 0) {
            total_in += got;
            read_count++;
            if (read_count <= 10) { /* proves whether the mic delivers audio in real time */
                char read_msg[120];
                snprintf(read_msg, sizeof(read_msg), "UAC_READ #%lu got=%lu wait_us=%lld total=%lu",
                         (unsigned long)read_count, (unsigned long)got,
                         (long long)(read_end_us - read_start_us), (unsigned long)total_in);
                ble_status(read_msg);
            }
            now = esp_timer_get_time();
            if (first_byte_us == 0) first_byte_us = now;
            last_byte_us = now;

            if (adpcm_mode) {
                const int16_t *in = (const int16_t *)chunk;
                const uint32_t nfr = got / frame_bytes;
                for (uint32_t i = 0; i < nfr; i++) {
                    float mono = 0.0f;
                    for (uint32_t c = 0; c < fmt.channels; c++) mono += (float)in[i * fmt.channels + c];
                    mono /= (float)fmt.channels;
                    float y;
                    if (fir_push(&s_fir, mono, &y)) {
                        y = y * (float)DIGITAL_GAIN_X100 / 100.0f;
                        if (y > 32767.0f) y = 32767.0f;
                        if (y < -32768.0f) y = -32768.0f;
                        adpcm_feed(&enc, (int16_t)lrintf(y));
                    }
                }
                continue;
            }
            uint8_t *dst = stream_mode ? outbuf : rec + written;
            uint32_t out_bytes;
            if (decim == 1) {
                memcpy(dst, chunk, got);
                out_bytes = got;
            } else {
                const int16_t *in = (const int16_t *)chunk;
                int16_t *out = (int16_t *)dst;
                const uint32_t nfr = got / frame_bytes;
                for (uint32_t i = 0; i < nfr; i++) {
                    for (uint32_t c = 0; c < fmt.channels; c++) acc[c] += in[i * fmt.channels + c];
                    if (++acc_cnt == decim) {
                        for (uint32_t c = 0; c < fmt.channels; c++) {
                            *out++ = (int16_t)(acc[c] / (int32_t)decim);
                            acc[c] = 0;
                        }
                        acc_cnt = 0;
                    }
                }
                out_bytes = (uint32_t)((uint8_t *)out - dst);
            }
            if (stream_mode) {
                size_t sent = xStreamBufferSend(fw.sb, outbuf, out_bytes, 0);
                if (sent < out_bytes) fifo_dropped += (uint32_t)(out_bytes - sent);
                written += (uint32_t)sent;
            } else {
                written += out_bytes;
            }
        } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "uac_host_device_read: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (!s_mic_gone) uac_host_device_stop(h);
    streaming = false;

    uint32_t data_bytes;
    if (stream_mode) {
        fw.stop = true;
        xSemaphoreTake(fw.done, pdMS_TO_TICKS(20000));
        data_bytes = fw.written - (fw.written % frame_bytes);
        vStreamBufferDelete(fw.sb);
        vSemaphoreDelete(fw.done);
    } else {
        if (adpcm_mode) {
            adpcm_finish(&enc);
            written = (uint32_t)(enc.wp - rec);
        }
        data_bytes = written - (written % frame_bytes);
        if (data_bytes > max_bytes) { /* flash smaller than the capture */
            data_bytes = max_bytes - (max_bytes % frame_bytes);
            ble_status("FLASH_CAP_TRUNCATED");
        }
        if (adpcm_mode) data_bytes -= data_bytes % ADPCM_BLOCK_BYTES;
        for (uint32_t off = 0; off < data_bytes; ) {
            uint32_t n = data_bytes - off > 4096 ? 4096 : data_bytes - off;
            if (fwrite(rec + off, 1, n, f) != n) {
                ESP_LOGE(TAG, "Flash write error");
                data_bytes = off - (off % frame_bytes);
                break;
            }
            off += n;
        }
    }
    const uint32_t total_frames = adpcm_mode
        ? (data_bytes / ADPCM_BLOCK_BYTES) * ADPCM_SAMPLES_PER_BLOCK
        : data_bytes / frame_bytes;

    /* Measured elapsed time = from first real audio byte to last real audio
     * byte. This excludes the mic-open/USB-negotiation delay so the rate
     * calc isn't skewed by startup latency. */
    double elapsed_s = (last_byte_us > first_byte_us)
                        ? (double)(last_byte_us - first_byte_us) / 1000000.0
                        : (double)rec_seconds;
    uint32_t effective_rate = (elapsed_s > 0.05 && total_frames > 0)
                               ? (uint32_t)((double)total_frames / elapsed_s + 0.5)
                               : fmt.sample_rate;

    ESP_LOGI(TAG, "Negotiated rate=%lu Hz, MEASURED effective rate=%lu Hz over %.3f s "
                  "(%lu frames, %lu bytes)",
             (unsigned long)fmt.sample_rate, (unsigned long)effective_rate, elapsed_s,
             (unsigned long)total_frames, (unsigned long)data_bytes);
    {
        char rate_msg[170];
        snprintf(rate_msg, sizeof(rate_msg),
                 "RATE negotiated=%luHz hdr=%luHz decim=%lu out_frames_per_s=%lu frames=%lu bytes=%lu fifo_dropped=%lu adpcm=%d",
                 (unsigned long)fmt.sample_rate, (unsigned long)hdr_rate, (unsigned long)decim,
                 (unsigned long)effective_rate,
                 (unsigned long)total_frames, (unsigned long)data_bytes,
                 (unsigned long)fifo_dropped, (int)adpcm_mode);
        ble_status(rate_msg);
    }

    if (adpcm_mode) {
        adpcm_make_header(hdr, data_bytes, hdr_rate, total_frames);
    } else {
        wav_make_header(hdr, data_bytes, hdr_rate, fmt.channels, bits);
    }
    if (fseek(f, 0, SEEK_SET) == 0 && fwrite(hdr, 1, hdr_len, f) == hdr_len) {
        ESP_LOGI(TAG, "Saved %lu bytes of audio (header rate=%lu Hz)",
                 (unsigned long)data_bytes, (unsigned long)hdr_rate);
        ok = data_bytes > 0;
        if (!ok) s_rec_err = "no_audio_data";
    } else {
        s_rec_err = "finalize";
        ESP_LOGE(TAG, "Could not finalize WAV header");
    }

done:
    if (f != NULL) fclose(f);
    if (streaming && !s_mic_gone) uac_host_device_stop(h);
    if (h != NULL) uac_host_device_close(h);
    free(chunk);
    free(outbuf);
    if (rec) heap_caps_free(rec);
    return ok;
}

/* ------------------------------------------------------------------ */
/* BLE GATT server                                                      */
/* ------------------------------------------------------------------ */

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        attr_handle == s_control_chr_val_handle) {
        uint8_t val = 0x01; /* default to upload if no payload, keeps old behavior */
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &val, 1, NULL);
        }
        if (val == 0x02) {
            ESP_LOGI(TAG, "Control write 0x02 -> RECORD requested");
            s_record_requested = true;
        } else {
            ESP_LOGI(TAG, "Control write 0x01 -> UPLOAD requested");
            s_upload_requested = true;
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CONTROL_CHR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_control_chr_val_handle,
            },
            {
                .uuid = &DATA_CHR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_INDICATE,
                .val_handle = &s_data_chr_val_handle,
            },
            {
                .uuid = &LOG_CHR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_log_chr_val_handle,
            },
            { 0 } /* terminator */
        },
    },
    { 0 } /* terminator */
};

static void ble_advertise(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected, handle=%d", s_conn_handle);
        } else {
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        s_upload_requested = false;
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_data_chr_val_handle) {
            s_notify_enabled = event->subscribe.cur_indicate;
            ESP_LOGI(TAG, "Indicate %s", s_notify_enabled ? "ENABLED" : "DISABLED");
        } else if (event->subscribe.attr_handle == s_log_chr_val_handle) {
            s_log_notify_enabled = event->subscribe.cur_notify;
            /* No history replay -- status strings are one-off checkpoints,
             * not a log stream, so there's nothing to flush. */
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_att_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU negotiated: %u", s_att_mtu);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        /* Fires for both notifications and indications; only indications
         * carry a real per-message ack, which is what we rely on below. */
        if (event->notify_tx.indication) {
            s_indicate_status = event->notify_tx.status;
            s_indicate_pending = false;
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
}

static void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool ble_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return false;

    if (nimble_port_init() != ESP_OK) return false;

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    if (ble_gatts_count_cfg(gatt_svr_svcs) != 0) return false;
    if (ble_gatts_add_svcs(gatt_svr_svcs) != 0) return false;

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    nimble_port_freertos_init(ble_host_task);
    return true;
}

/* ------------------------------------------------------------------ */
/* BLE status channel -- sends a handful of EXPLICIT, hand-picked status  */
/* strings at real checkpoints only (boot, mic detect, record done,       */
/* upload done, ...). This deliberately does NOT hook esp_log: the USB    */
/* host / UAC stack emits dozens of ESP_LOG lines in a burst during       */
/* enumeration, and mirroring every one of those as an immediate BLE      */
/* notify was flooding the link's tiny outbound queue and crashing the    */
/* NimBLE stack the instant a client subscribed (hence the instant,       */
/* silent disconnects with zero data ever seen on the PC side). Native    */
/* ESP_LOG output to the console/UART is completely unaffected -- this    */
/* only controls what additionally goes out over BLE.                    */
/* ------------------------------------------------------------------ */

#define STATUS_MAX_LEN 180

static void ble_status(const char *msg)
{
    printf("%s\n", msg); /* still visible on the native console/UART */

    size_t len = strlen(msg);
    if (len > STATUS_MAX_LEN) len = STATUS_MAX_LEN;

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_log_notify_enabled ||
        s_log_chr_val_handle == 0) {
        return; /* nobody listening right now -- fine, these are one-off
                    checkpoints, not a stream that needs buffering */
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat((const uint8_t *)msg, len);
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn_handle, s_log_chr_val_handle, om);
    }
}

/* Heartbeat: one small line every 2 s while a client is subscribed, so a
 * client that connects LATE still sees the current health of every stage
 * (this is a single tiny notify, not a log stream). */
static void diag_task(void *arg)
{
    char msg[160];
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (strcmp((const char *)s_stage, "upload") == 0) continue; /* don't compete with the transfer */
        snprintf(msg, sizeof(msg),
                 "DIAG stage=%s fs=%d usb=%d mic=%d spiram_cfg=%d psram=%u/%u int_free=%u err=%s\n",
                 s_stage, s_fs_ok, s_usb_ok, s_mic_seen,
#ifdef CONFIG_SPIRAM
                 1,
#else
                 0,
#endif
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 s_rec_err);
        ble_status(msg);
    }
}

/* ------------------------------------------------------------------ */
/* Upload over BLE indications (acked, reliable)                        */
/* ------------------------------------------------------------------ */

/* Sends one indication and blocks until the peer acks it (or times out).
 * Returns true only on a confirmed, successful delivery. */
static bool send_indication(const uint8_t *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) return false;

    s_indicate_status = -1;
    s_indicate_pending = true;

    int rc = ble_gatts_indicate_custom(s_conn_handle, s_data_chr_val_handle, om);
    if (rc != 0) {
        /* BLE_HS_ENOMEM / BLE_HS_EBUSY (previous indication still in flight)
         * or any other error: nothing was queued, caller should retry. */
        s_indicate_pending = false;
        return false;
    }

    const int64_t start = esp_timer_get_time();
    const int64_t ack_timeout_us = 3 * 1000000LL;
    while (s_indicate_pending) {
        if ((esp_timer_get_time() - start) > ack_timeout_us) {
            ESP_LOGW(TAG, "indicate ack timed out");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_indicate_status != 0) {
        ESP_LOGW(TAG, "indicate nacked/failed, status=%d", s_indicate_status);
        return false;
    }
    return true;
}

/* Retries a single chunk a bounded number of times before giving up. */
static bool send_indication_with_retries(const uint8_t *data, size_t len, int max_attempts)
{
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (send_indication(data, len)) return true;
        vTaskDelay(pdMS_TO_TICKS(30 * (attempt + 1))); /* mild backoff */
    }
    return false;
}

static bool ble_upload_wav(void)
{
    FILE *file = fopen(RECORD_PATH, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "No recording to upload");
        return false;
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(file);
        return false;
    }

    /* Size-header packet first: 4 bytes little-endian file size. Must be
     * acked before we start sending data, same as every chunk after it. */
    uint8_t size_hdr[4];
    put32(size_hdr, (uint32_t)file_size);
    if (!send_indication_with_retries(size_hdr, sizeof(size_hdr), 5)) {
        ESP_LOGE(TAG, "Failed to send size header");
        fclose(file);
        return false;
    }

    const uint16_t chunk_size = (s_att_mtu > 3) ? (s_att_mtu - 3) : 20;
    uint8_t *buf = malloc(chunk_size);
    if (buf == NULL) {
        fclose(file);
        return false;
    }

    size_t total_sent = 0;
    bool ok = true;
    while (total_sent < (size_t)file_size) {
        size_t remaining = (size_t)file_size - total_sent;
        size_t to_read = remaining > chunk_size ? chunk_size : remaining;
        size_t n = fread(buf, 1, to_read, file);
        if (n == 0) { ok = false; break; }

        if (!send_indication_with_retries(buf, n, 8)) {
            ESP_LOGE(TAG, "Giving up on chunk at offset %u after retries", (unsigned)total_sent);
            ok = false;
            break;
        }
        total_sent += n;
    }

    free(buf);
    fclose(file);
    ESP_LOGI(TAG, "Uploaded %u / %ld bytes over BLE (acked)", (unsigned)total_sent, file_size);
    return ok && total_sent == (size_t)file_size;
}

/* ------------------------------------------------------------------ */
/* MAIN                                                                 */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    const gpio_config_t motor_cfg = {
        .pin_bit_mask = (1ULL << MOTOR_IN1) | (1ULL << MOTOR_IN2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&motor_cfg);
    motor_stop();

    motor_run();  /* startup self-test blip */
    vTaskDelay(pdMS_TO_TICKS(1000));
    motor_stop();

    /* BLE first, so diagnostics are reachable even if everything else fails. */
    if (!ble_init()) {
        ESP_LOGE(TAG, "BLE init failed");
        for (;;) motor_pulses(PULSE_BLE_INIT_FAILED), vTaskDelay(pdMS_TO_TICKS(3000));
    }
    xTaskCreate(diag_task, "diag", 3072, NULL, 4, NULL);
    ble_status("BOOT_OK");

    s_fs_ok = littlefs_init();
    if (!s_fs_ok) {
        ble_status("FS_INIT_FAILED");
        motor_pulses(PULSE_RECORD_FAILED);
        /* keep going: BLE diagnostics must stay alive */
    }

    s_evt_q = xQueueCreate(4, sizeof(mic_event_t));
    s_usb_ok = (s_evt_q != NULL) && usb_mic_init();
    if (!s_usb_ok) {
        ESP_LOGE(TAG, "USB/UAC init failed");
        motor_pulses(PULSE_USB_INIT_FAILED);
        ble_status("USB_INIT_FAILED");
    }

    ESP_LOGI(TAG, "Boot settle: waiting %d ms before auto-record...", BOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
    s_record_requested = true; /* fully automatic: no BLE command needed */
    int64_t retry_at_us = 0;   /* after a failure, retry so a late client still sees it */

    while (true) {
        if (retry_at_us && esp_timer_get_time() > retry_at_us) {
            retry_at_us = 0;
            s_record_requested = true;
        }
        if (s_record_requested) {
            s_record_requested = false;
            s_stage = "wait_mic";
            ble_status("WAITING_FOR_MIC");
            mic_event_t ev;
            if (s_evt_q != NULL &&
                xQueueReceive(s_evt_q, &ev, pdMS_TO_TICKS(MIC_DETECT_TIMEOUT_MS)) == pdTRUE) {
                s_mic_seen = true;
                ble_status("MIC_DETECTED");
                motor_pulses(PULSE_RECORD_STARTED);
                bool ok = record_wav(&ev);
                motor_pulses(ok ? PULSE_RECORD_SAVED : PULSE_RECORD_FAILED);
                ble_status(ok ? "RECORD_OK" : "RECORD_FAILED");
                s_upload_requested = ok;
                s_stage = ok ? "recorded" : "record_failed";
                if (!ok) retry_at_us = esp_timer_get_time() + 15000000LL;
            } else {
                s_rec_err = "mic_timeout";
                s_stage = "mic_timeout";
                motor_pulses(PULSE_RECORD_FAILED);
                ble_status("MIC_TIMEOUT");
                retry_at_us = esp_timer_get_time() + 15000000LL;
            }
        }

        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_notify_enabled && s_upload_requested) {
            motor_pulses(PULSE_UPLOAD_STARTED);
            s_stage = "upload";
            ble_status("UPLOAD_START");
            bool ok = ble_upload_wav();
            motor_pulses(ok ? PULSE_UPLOAD_DONE : PULSE_RECORD_FAILED);
            s_upload_requested = false; /* one-shot: don't resend automatically */
            /* Explicit, unambiguous end-of-transfer marker -- the PC side
             * treats this exact string as authoritative "done" and stops
             * waiting, instead of guessing from a timeout or byte count. */
            s_stage = "done";
            ble_status(ok ? "XFER_COMPLETE status=OK" : "XFER_COMPLETE status=FAIL");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}