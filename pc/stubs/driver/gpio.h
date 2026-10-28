#pragma once
#include <stdint.h>
typedef int gpio_num_t;
#define GPIO_NUM_0 0
#define GPIO_NUM_4 4
#define GPIO_NUM_17 17
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#define GPIO_NUM_9 9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
typedef struct { uint64_t pin_bit_mask; int mode; int pull_up_en; } gpio_config_t;
int gpio_config(const gpio_config_t *c);
int gpio_get_level(gpio_num_t g);
typedef enum { GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY } gpio_pull_mode_t;
#define GPIO_MODE_OUTPUT 2
int gpio_reset_pin(gpio_num_t g);
int gpio_set_direction(gpio_num_t g, int m);
int gpio_set_level(gpio_num_t g, int l);
int gpio_set_pull_mode(gpio_num_t g, gpio_pull_mode_t m);
