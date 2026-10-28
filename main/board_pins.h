#pragma once
// ESP32-S3 N16R8 pin map.
// Never use: GPIO26-32 (flash), 35-37 (octal PSRAM), 19/20 (USB mic host),
// 0/3/45/46 (strapping), 43/44 (UART0 console).
#include "driver/gpio.h"
#include "driver/spi_common.h"

#define BOARD_SPI_HOST   SPI2_HOST

// Shared SPI bus (TFT + SD)
#define PIN_SPI_MOSI     GPIO_NUM_11   // display SDA
#define PIN_SPI_SCLK     GPIO_NUM_12   // display SCL
#define PIN_SPI_MISO     GPIO_NUM_13   // SD only (TFT has no MISO); this is the chip's native MISO pin

// TFT (the pin set that is known to work)
#define PIN_TFT_CS       GPIO_NUM_10
#define PIN_TFT_RST      GPIO_NUM_9
#define PIN_TFT_DC       GPIO_NUM_8    // display A0
#define PIN_TFT_BL       GPIO_NUM_17   // display LED pin is wired straight to 3V3 (GPIO13 is the SD MISO)

// SD card module (moved: GPIO10 and GPIO13 are now used by the TFT)
#define PIN_SD_CS        GPIO_NUM_15

// PN532 on I2C
#define PIN_NFC_SDA      GPIO_NUM_4
#define PIN_NFC_SCL      GPIO_NUM_6
#define PIN_NFC_IRQ      GPIO_NUM_5

// Bring-up only (step 1): on-board BOOT button toggles a test recording.
#define PIN_BOOT_BTN     GPIO_NUM_0

// Proposed, not used in step 1
#define PIN_BTN_STOP     GPIO_NUM_16   // active low, internal pull-up
#define PIN_BAT_ADC      GPIO_NUM_7    // ADC1 so it works with Wi-Fi on
