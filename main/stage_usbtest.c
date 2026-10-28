// USB host diagnostic: reports EVERY device that enumerates (any class), with speed, IDs,
// and the audio class/protocol of each interface. No UAC driver is installed, so nothing
// else can claim the device. Tells "nothing enumerates" apart from "enumerates but is not UAC 1.0".
#include "stages.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "config_store.h"
#include "sd_card.h"
#include "sd_log.h"

static const char *TAG = "usbtest";

static volatile bool s_new_dev;
static volatile uint8_t s_new_addr;
static volatile bool s_gone;

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags) ESP_LOGI(TAG, "USB library event flags 0x%02lx", (unsigned long)flags);
    }
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = msg->new_dev.address;
        s_new_dev = true;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_gone = true;
    }
}

static void print_str(const char *label, const usb_str_desc_t *s)
{
    char buf[64] = "";
    if (s && s->bLength > 2) {
        int n = (s->bLength - 2) / 2;
        if (n > 62) n = 62;
        for (int i = 0; i < n; i++) buf[i] = (char)(s->wData[i] & 0x7F);
        buf[n] = 0;
    }
    ESP_LOGI(TAG, "  %s: \"%s\"", label, buf);
}

static void describe_device(usb_host_client_handle_t client, uint8_t addr)
{
    usb_device_handle_t dev;
    esp_err_t e = usb_host_device_open(client, addr, &dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "device open failed: %s", esp_err_to_name(e)); return; }

    usb_device_info_t info;
    usb_host_device_info(dev, &info);
    const char *speed = info.speed == USB_SPEED_LOW ? "LOW (1.5 Mbps)"
                      : info.speed == USB_SPEED_FULL ? "FULL (12 Mbps)" : "HIGH";
    ESP_LOGI(TAG, "=== DEVICE ENUMERATED: address %u, speed %s ===", info.dev_addr, speed);
    print_str("manufacturer", info.str_desc_manufacturer);
    print_str("product     ", info.str_desc_product);

    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK) {
        ESP_LOGI(TAG, "  VID 0x%04X PID 0x%04X, USB %x.%02x, device class 0x%02X (0x00 = see interfaces, 0xEF = composite)",
                 dd->idVendor, dd->idProduct, dd->bcdUSB >> 8, dd->bcdUSB & 0xFF, dd->bDeviceClass);
    }

    const usb_config_desc_t *cd;
    if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK) {
        const uint8_t *p = (const uint8_t *)cd;
        const uint16_t total = cd->wTotalLength;
        bool uac1_stream = false, uac2 = false;
        for (uint16_t off = 0; off + 2 <= total && p[off] >= 2; off += p[off]) {
            if (p[off + 1] == 0x04 && p[off] >= 9) {                 // interface descriptor
                uint8_t num = p[off + 2], alt = p[off + 3], eps = p[off + 4];
                uint8_t cls = p[off + 5], sub = p[off + 6], proto = p[off + 7];
                const char *what = "";
                if (cls == 0x01) {
                    what = sub == 0x01 ? "audio CONTROL" : sub == 0x02 ? "audio STREAMING" : "audio";
                    if (proto == 0x20) uac2 = true;
                    if (sub == 0x02 && proto == 0x00 && eps > 0) uac1_stream = true;
                } else if (cls == 0x03) what = "HID";
                else if (cls == 0x08) what = "mass storage";
                else if (cls == 0x0A) what = "CDC data";
                else if (cls == 0x02) what = "CDC control";
                else if (cls == 0xFF) what = "vendor specific";
                ESP_LOGI(TAG, "  interface %u alt %u: class 0x%02X sub 0x%02X proto 0x%02X, %u endpoint(s)  %s%s",
                         num, alt, cls, sub, proto, eps, what, (cls == 0x01 && proto == 0x20) ? " (UAC 2.0)" : "");
            } else if (p[off + 1] == 0x05 && p[off] >= 7) {          // endpoint descriptor
                ESP_LOGI(TAG, "      endpoint 0x%02X, type %u, max packet %u", p[off + 2], p[off + 3] & 3,
                         p[off + 4] | (p[off + 5] << 8));
            }
        }
        if (uac2)
            ESP_LOGW(TAG, "=> This mic uses USB Audio Class 2.0. The ESP32 UAC host driver only supports class 1.0 (UAC 1.0).");
        else if (uac1_stream)
            ESP_LOGI(TAG, "=> UAC 1.0 audio streaming interface found: the audio driver should accept this device.");
        else
            ESP_LOGW(TAG, "=> No UAC 1.0 audio streaming interface in the active configuration.");
    }
    usb_host_device_close(client, dev);
}

void stage_usbtest(void)
{
    config_init();
    sd_log_init(config_next_boot_count());      // results are also saved to /sdcard/logs/boot_NNNNN.txt
    board_spi_init();
    sd_card_mount();
    esp_log_level_set("USBH", ESP_LOG_DEBUG);
    esp_log_level_set("HUB", ESP_LOG_DEBUG);
    esp_log_level_set("HCD DWC", ESP_LOG_DEBUG);
    esp_log_level_set("USB HOST", ESP_LOG_DEBUG);

    const usb_host_config_t host_cfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t e = usb_host_install(&host_cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(e)); return; }
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 6, NULL);

    usb_host_client_handle_t client;
    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    e = usb_host_client_register(&cc, &client);
    if (e != ESP_OK) { ESP_LOGE(TAG, "client register: %s", esp_err_to_name(e)); return; }

    ESP_LOGI(TAG, "USB host running on GPIO19 (D-) / GPIO20 (D+). Plug the mic in now (or unplug and replug it).");
    int idle = 0;
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(500));
        if (s_new_dev) { s_new_dev = false; idle = 0; describe_device(client, s_new_addr); }
        if (s_gone) { s_gone = false; ESP_LOGW(TAG, "device disconnected"); }
        if (++idle % 10 == 0) {
            usb_host_lib_info_t li;
            usb_host_lib_info(&li);
            ESP_LOGI(TAG, "still waiting: %d device(s) enumerated, no new connection (%d s)", li.num_devices, idle / 2);
        }
    }
}
