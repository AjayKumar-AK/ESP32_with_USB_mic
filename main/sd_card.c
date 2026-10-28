#include "sd_card.h"
#include "board_pins.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;
static bool s_bus_ready;

esp_err_t board_spi_init(void)
{
    if (s_bus_ready) return ESP_OK;

    // Keep both devices deselected while the bus comes up.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_CS) | (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_TFT_CS, 1);
    gpio_set_level(PIN_SD_CS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUS_MAX_XFER_BYTES,
    };
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    s_bus_ready = true;
    return ESP_OK;
}

static esp_err_t mount_once(void)
{
    if (s_card) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   // never auto-format a teacher's data
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = SD_SPI_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = BOARD_SPI_HOST;

    ESP_LOGI(TAG, "mounting SD: CS GPIO%d, MOSI GPIO%d, MISO GPIO%d, SCK GPIO%d, %d kHz",
             (int)PIN_SD_CS, (int)PIN_SPI_MOSI, (int)PIN_SPI_MISO, (int)PIN_SPI_SCLK, SD_SPI_FREQ_KHZ);
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (0x108 = card silent: check card power, CS/MISO wires, FAT32)", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sd_card_mount_retry(int attempts)
{
    if (s_card) return ESP_OK;
    esp_err_t err = ESP_FAIL;
    for (int i = 1; i <= attempts; i++) {
        err = mount_once();
        if (err == ESP_OK) return ESP_OK;
        if (i < attempts) {
            ESP_LOGW(TAG, "SD mount attempt %d/%d failed, retrying", i, attempts);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
    return err;
}

esp_err_t sd_card_mount(void) { return sd_card_mount_retry(SD_MOUNT_ATTEMPTS); }

void sd_card_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
}

bool sd_card_is_mounted(void) { return s_card != NULL; }

esp_err_t sd_card_free_bytes(uint64_t *total, uint64_t *free_bytes)
{
    return esp_vfs_fat_info(SD_MOUNT_POINT, total, free_bytes);
}
