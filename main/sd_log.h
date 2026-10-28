#pragma once
#include <stdint.h>

// Copies everything the firmware logs (the same lines you see on the serial console) into
// /sdcard/logs/boot_NNNNN.txt, so the logs survive without a cable. Call it as early as possible:
// lines logged before the SD card mounts are kept (first 16 KB) and written once it is available.
void sd_log_init(uint32_t boot_number);
