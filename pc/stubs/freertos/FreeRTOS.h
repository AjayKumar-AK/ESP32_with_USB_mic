#pragma once
#include <stdint.h>
typedef uint32_t TickType_t; typedef int BaseType_t; typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portMAX_DELAY 0xFFFFFFFFu
