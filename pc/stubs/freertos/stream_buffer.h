#pragma once
#include "FreeRTOS.h"
#include <stddef.h>
typedef void *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t s, size_t t, int caps);
StreamBufferHandle_t xStreamBufferCreate(size_t s, size_t t);
size_t xStreamBufferSend(StreamBufferHandle_t h, const void *d, size_t n, TickType_t t);
size_t xStreamBufferReceive(StreamBufferHandle_t h, void *d, size_t n, TickType_t t);
typedef struct { int x; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
int xPortInIsrContext(void);
uint32_t xTaskGetTickCount(void);
#define configTICK_RATE_HZ 1000
