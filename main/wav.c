#include "wav.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

void wav_fill_header(uint8_t h[WAV_HEADER_BYTES], uint32_t rate,
                     uint16_t ch, uint16_t bits, uint32_t data_bytes)
{
    uint16_t block = (uint16_t)(ch * bits / 8);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    put16(h + 20, 1);              // PCM
    put16(h + 22, ch);
    put32(h + 24, rate);
    put32(h + 28, rate * block);
    put16(h + 32, block);
    put16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

bool wav_patch_sizes(FILE *f, uint32_t data_bytes)
{
    uint8_t b[4];
    put32(b, 36 + data_bytes);
    if (fseek(f, 4, SEEK_SET) != 0 || fwrite(b, 1, 4, f) != 4) return false;
    put32(b, data_bytes);
    if (fseek(f, 40, SEEK_SET) != 0 || fwrite(b, 1, 4, f) != 4) return false;
    return fflush(f) == 0;
}
