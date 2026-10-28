#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Parks both chip selects high, then brings up the shared SPI2 bus.
esp_err_t board_spi_init(void);
// Mounts the FAT card at SD_MOUNT_POINT over SPI.
esp_err_t sd_card_mount(void);                  // SD_MOUNT_ATTEMPTS tries, 400 ms apart
esp_err_t sd_card_mount_retry(int attempts);
void sd_card_unmount(void);
bool sd_card_is_mounted(void);
esp_err_t sd_card_free_bytes(uint64_t *total, uint64_t *free_bytes);
