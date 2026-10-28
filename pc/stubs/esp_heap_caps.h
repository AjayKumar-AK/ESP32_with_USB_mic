#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_DMA 4
#include <stdint.h>
void *heap_caps_malloc(size_t s, int c);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(int c);
size_t heap_caps_get_minimum_free_size(int c);
