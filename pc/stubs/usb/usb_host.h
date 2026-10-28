#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define ESP_INTR_FLAG_LEVEL1 2
#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS 1
typedef void *usb_host_client_handle_t; typedef void *usb_device_handle_t;
typedef struct { bool skip_phy_setup; int intr_flags; } usb_host_config_t;
typedef enum { USB_HOST_CLIENT_EVENT_NEW_DEV, USB_HOST_CLIENT_EVENT_DEV_GONE } usb_host_client_event_t;
typedef struct { usb_host_client_event_t event; union { struct { uint8_t address; } new_dev; struct { usb_device_handle_t dev_hdl; } dev_gone; }; } usb_host_client_event_msg_t;
typedef void (*usb_host_client_event_cb_t)(const usb_host_client_event_msg_t *, void *);
typedef struct { bool is_synchronous; int max_num_event_msg; struct { usb_host_client_event_cb_t client_event_callback; void *callback_arg; } async; } usb_host_client_config_t;
typedef enum { USB_SPEED_LOW, USB_SPEED_FULL, USB_SPEED_HIGH } usb_speed_t;
typedef struct { uint8_t bLength, bDescriptorType; uint16_t wData[1]; } usb_str_desc_t;
typedef struct { usb_speed_t speed; uint8_t dev_addr, bMaxPacketSize0, bConfigurationValue; const usb_str_desc_t *str_desc_manufacturer, *str_desc_product, *str_desc_serial_num; } usb_device_info_t;
typedef struct { uint16_t bcdUSB; uint8_t bDeviceClass; uint16_t idVendor, idProduct; } usb_device_desc_t;
typedef struct { uint16_t wTotalLength; } usb_config_desc_t;
typedef struct { int num_devices, num_clients; } usb_host_lib_info_t;
esp_err_t usb_host_install(const usb_host_config_t *c);
esp_err_t usb_host_lib_handle_events(TickType_t t, uint32_t *f);
esp_err_t usb_host_lib_info(usb_host_lib_info_t *i);
esp_err_t usb_host_client_register(const usb_host_client_config_t *c, usb_host_client_handle_t *h);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t h, TickType_t t);
esp_err_t usb_host_device_open(usb_host_client_handle_t c, uint8_t a, usb_device_handle_t *d);
esp_err_t usb_host_device_close(usb_host_client_handle_t c, usb_device_handle_t d);
esp_err_t usb_host_device_info(usb_device_handle_t d, usb_device_info_t *i);
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t d, const usb_device_desc_t **o);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t d, const usb_config_desc_t **o);
