#include "nfc_pn532.h"
#include "app_config.h"
#include "app_events.h"
#include "board_pins.h"
#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nfc";
#define PN532_ADDR 0x24

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static volatile bool s_ok;

static esp_err_t pn_write(const uint8_t *d, size_t n) { return i2c_master_transmit(s_dev, d, n, 100); }
static esp_err_t pn_read(uint8_t *d, size_t n) { return i2c_master_receive(s_dev, d, n, 100); }

// The first byte of every read is a status byte: bit 0 set = response ready.
static bool pn_wait_ready(int timeout_ms)
{
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t st = 0;
    do {
        if (pn_read(&st, 1) == ESP_OK && (st & 1)) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (esp_timer_get_time() < end);
    return false;
}

static esp_err_t pn_send(const uint8_t *cmd, size_t n)
{
    uint8_t f[40];
    size_t i = 0;
    const uint8_t len = (uint8_t)(n + 1);
    f[i++] = 0x00; f[i++] = 0x00; f[i++] = 0xFF;          // preamble + start code
    f[i++] = len;  f[i++] = (uint8_t)(~len + 1);          // length + checksum
    f[i++] = 0xD4;                                         // host -> PN532
    uint8_t sum = 0xD4;
    for (size_t k = 0; k < n; k++) { f[i++] = cmd[k]; sum += cmd[k]; }
    f[i++] = (uint8_t)(~sum + 1);                          // data checksum
    f[i++] = 0x00;                                         // postamble
    return pn_write(f, i);
}

// Sends a command, checks the ACK, and returns the response payload (after D5 <cmd+1>).
static bool pn_cmd(const uint8_t *cmd, size_t n, uint8_t *resp, size_t cap, size_t *rlen, int timeout_ms)
{
    if (pn_send(cmd, n) != ESP_OK) return false;
    if (!pn_wait_ready(100)) return false;
    uint8_t ack[7];
    if (pn_read(ack, sizeof(ack)) != ESP_OK) return false;
    static const uint8_t k_ack[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    if (memcmp(ack + 1, k_ack, sizeof(k_ack)) != 0) return false;

    if (!pn_wait_ready(timeout_ms)) return false;
    uint8_t b[32];
    if (pn_read(b, sizeof(b)) != ESP_OK) return false;
    if (!(b[0] & 1) || b[1] != 0x00 || b[2] != 0x00 || b[3] != 0xFF) return false;
    const uint8_t len = b[4];
    if ((uint8_t)(len + b[5]) != 0) return false;           // length checksum
    if (len < 2 || b[6] != 0xD5 || b[7] != (uint8_t)(cmd[0] + 1)) return false;
    const size_t dl = (size_t)len - 2;
    if (dl > cap || 8 + dl > sizeof(b)) return false;
    if (dl) memcpy(resp, b + 8, dl);
    if (rlen) *rlen = dl;
    return true;
}

esp_err_t nfc_init(void)
{
    if (!s_bus) {
        i2c_master_bus_config_t bc = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = PIN_NFC_SDA,
            .scl_io_num = PIN_NFC_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
        if (e != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(e)); return e; }
        const i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PN532_ADDR,
            .scl_speed_hz = 100000,
        };
        e = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
        if (e != ESP_OK) { ESP_LOGE(TAG, "i2c device: %s", esp_err_to_name(e)); return e; }
    }

    uint8_t r[8];
    size_t n = 0;
    bool up = false;
    for (int attempt = 0; attempt < 5 && !up; attempt++) {
        const uint8_t fw[] = {0x02};                                   // GetFirmwareVersion
        up = pn_cmd(fw, sizeof(fw), r, sizeof(r), &n, 200) && n >= 4 && r[0] == 0x32;
        if (!up) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!up) {
        s_ok = false;
        ESP_LOGW(TAG, "PN532 not answering (check wiring, I2C mode switches, 3.3 V)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PN532 firmware %u.%u", r[1], r[2]);

    const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};                    // SAMConfiguration: normal mode
    pn_cmd(sam, sizeof(sam), r, sizeof(r), &n, 200);
    const uint8_t retries[] = {0x32, 0x05, 0xFF, 0x01, 0x02};          // few passive retries: InList returns fast
    pn_cmd(retries, sizeof(retries), r, sizeof(r), &n, 200);
    s_ok = true;
    return ESP_OK;
}

bool nfc_ok(void) { return s_ok; }

// Drains anything the PN532 still wants to send after a failed exchange.
static void pn_flush(void)
{
    uint8_t b[32];
    pn_read(b, sizeof(b));
}

// 1 = card, 0 = none, -1 = communication error
static int nfc_poll(char *uid_hex, size_t cap)
{
    const uint8_t in_list[] = {0x4A, 0x01, 0x00};                      // 1 target, 106 kbps type A
    uint8_t r[24];
    size_t n = 0;
    if (!pn_cmd(in_list, sizeof(in_list), r, sizeof(r), &n, 150)) { pn_flush(); return -1; }
    if (n < 1 || r[0] == 0) return 0;
    if (n < 6) return -1;
    const size_t ulen = r[5];
    if (ulen == 0 || 6 + ulen > n || ulen * 2 + 1 > cap) return -1;
    for (size_t i = 0; i < ulen; i++) snprintf(uid_hex + i * 2, 3, "%02X", r[6 + i]);
    return 1;
}

static void nfc_task(void *arg)
{
    char last_uid[24] = "";
    bool latch = false;
    int fails = 0;
    int64_t last_seen = 0, next_init = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (!s_ok) {
            if (now >= next_init) {
                if (nfc_init() == ESP_OK) fails = 0;
                next_init = esp_timer_get_time() + 3 * 1000000LL;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        char uid[24];
        const int r = nfc_poll(uid, sizeof(uid));
        if (r < 0) {
            if (++fails >= 10) { s_ok = false; ESP_LOGW(TAG, "reader lost"); }
        } else {
            fails = 0;
            if (r == 1) {
                if (!latch || strcmp(uid, last_uid) != 0) {
                    ESP_LOGI(TAG, "tap: %s", uid);
                    app_event_t e = { .type = EVT_TAP };
                    strlcpy(e.uid, uid, sizeof(e.uid));
                    app_post_event(&e);
                    strlcpy(last_uid, uid, sizeof(last_uid));
                }
                latch = true;
                last_seen = now;
            } else if (latch && (now - last_seen) >= (int64_t)NFC_RELEASE_MS * 1000) {
                latch = false;
                last_uid[0] = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NFC_POLL_MS));
    }
}

void nfc_start_task(void)
{
    xTaskCreate(nfc_task, "nfc", 4096, NULL, 4, NULL);
}
