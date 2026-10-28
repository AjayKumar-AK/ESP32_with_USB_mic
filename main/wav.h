#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define WAV_HEADER_BYTES 44

void wav_fill_header(uint8_t hdr[WAV_HEADER_BYTES], uint32_t sample_rate,
                     uint16_t channels, uint16_t bits, uint32_t data_bytes);

// Rewrites only the RIFF and data size fields of an existing header.
bool wav_patch_sizes(FILE *f, uint32_t data_bytes);
