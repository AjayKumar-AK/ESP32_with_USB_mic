// USB mic handling taken from the working mic firmware: same USB host / UAC
// install, same alt-setting scoring for format selection, same read pattern.
// The only structural change: the device is opened once when it connects and
// the stream is started/stopped per recording.
#include "usb_mic.h"
#include "app_config.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

static const char *TAG = "usb_mic";

#define EV_CONNECTED BIT0

typedef enum { MSG_CONNECTED, MSG_GONE } msg_type_t;
typedef struct {
    msg_type_t type;
    uint8_t addr;
    uint8_t iface;
    uac_host_device_handle_t h;
} msg_t;

static uac_host_device_handle_t s_dev;
static mic_format_t s_fmt;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_q;
static EventGroupHandle_t s_ev;
static bool s_streaming;
static volatile bool s_mic_gone;
static volatile bool s_xfer_error;

/* ---------------------------------------------------------- format pick */
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

/* ------------------------------------------------------------ callbacks */
static void uac_driver_cb(uint8_t addr, uint8_t iface_num,
                          const uac_host_driver_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        msg_t m = { .type = MSG_CONNECTED, .addr = addr, .iface = iface_num };
        xQueueSend(s_q, &m, 0);
    }
}

static void uac_device_cb(uac_host_device_handle_t handle,
                          const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        s_mic_gone = true;
        msg_t m = { .type = MSG_GONE, .h = handle };
        xQueueSend(s_q, &m, 0);
    } else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        s_xfer_error = true;
    }
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

/* ------------------------------------------------------------- manager */
static void mic_mgr_task(void *arg)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;

        if (m.type == MSG_CONNECTED) {
            if (s_dev != NULL) continue;   // already have a mic

            const uint32_t ring_bytes = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
                                        ? UAC_RING_BUF_SIZE : (32 * 1024);
            const uac_host_device_config_t dc = {
                .addr = m.addr,
                .iface_num = m.iface,
                .buffer_size = ring_bytes,
                .buffer_threshold = UAC_RING_THRESHOLD,
                .callback = uac_device_cb,
                .callback_arg = NULL,
            };
            uac_host_device_handle_t h = NULL;
            esp_err_t err = uac_host_device_open(&dc, &h);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "uac_host_device_open: %s", esp_err_to_name(err));
                continue;
            }

            uac_host_dev_info_t info;
            memset(&info, 0, sizeof(info));
            if (uac_host_get_device_info(h, &info) == ESP_OK) {
                ESP_LOGI(TAG, "VID=0x%04X PID=0x%04X, %u alt setting(s)",
                         info.VID, info.PID, info.iface_alt_num);
            }

            mic_format_t fmt;
            memset(&fmt, 0, sizeof(fmt));
            if (!pick_format(h, info.iface_alt_num, &fmt)) {
                ESP_LOGE(TAG, "No usable audio format");
                uac_host_device_close(h);
                continue;
            }
            ESP_LOGI(TAG, "Using %u ch, %u-bit, %lu Hz", fmt.channels, fmt.bit_resolution,
                     (unsigned long)fmt.sample_rate);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_dev = h;
            s_fmt = fmt;
            s_streaming = false;
            s_mic_gone = false;
            s_xfer_error = false;
            xSemaphoreGive(s_lock);
            xEventGroupSetBits(s_ev, EV_CONNECTED);
            ESP_LOGI(TAG, "mic ready");
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool mine = (s_dev == m.h);
            if (mine) {
                s_dev = NULL;
                s_streaming = false;
                xEventGroupClearBits(s_ev, EV_CONNECTED);
            }
            xSemaphoreGive(s_lock);
            if (mine) {
                uac_host_device_close(m.h);
                ESP_LOGW(TAG, "mic disconnected");
            }
        }
    }
}

/* ----------------------------------------------------------------- API */
esp_err_t usb_mic_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(8, sizeof(msg_t));
    s_ev = xEventGroupCreate();
    if (!s_lock || !s_q || !s_ev) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
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
    err = uac_host_install(&uac_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreate(mic_mgr_task, "mic_mgr", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "USB host + UAC driver installed, waiting for mic");
    return ESP_OK;
}

bool usb_mic_is_connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & EV_CONNECTED);
}

bool usb_mic_wait_connected(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_CONNECTED, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return b & EV_CONNECTED;
}

esp_err_t usb_mic_get_format(mic_format_t *out)
{
    if (!usb_mic_is_connected()) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_fmt;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t usb_mic_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_dev || s_mic_gone) { xSemaphoreGive(s_lock); return ESP_ERR_NOT_FOUND; }
    if (s_streaming) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }

    s_xfer_error = false;
    const uac_host_stream_config_t stream_cfg = {
        .channels = s_fmt.channels,
        .bit_resolution = s_fmt.bit_resolution,
        .sample_freq = s_fmt.sample_rate,
        .flags = 0,
    };
    esp_err_t err = uac_host_device_start(s_dev, &stream_cfg);
    if (err == ESP_OK) {
        s_streaming = true;
    } else {
        ESP_LOGE(TAG, "uac_host_device_start: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

void usb_mic_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dev && s_streaming && !s_mic_gone) {
        uac_host_device_stop(s_dev);
    }
    s_streaming = false;
    xSemaphoreGive(s_lock);
}

bool usb_mic_has_fault(void)
{
    return s_mic_gone || s_xfer_error;
}

esp_err_t usb_mic_read(uint8_t *buf, uint32_t len, uint32_t *got, uint32_t timeout_ms)
{
    *got = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_dev || !s_streaming) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    // The UAC read timeout is in FreeRTOS ticks.
    esp_err_t e = uac_host_device_read(s_dev, buf, len, got, pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(s_lock);
    return e;
}
