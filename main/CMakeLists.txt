idf_component_register(
    SRCS "main.c" "stage_mic_test.c" "stage_display_demo.c" "stage_pintest.c" "stage_sdtest.c" "stage_usbtest.c" "stage_full_app.c"
         "app_ctrl.c" "config_store.c" "teachers.c" "wifi_mgr.c" "nfc_pn532.c" "button.c" "admin_ble.c" "uploader.c"
         "tft_st7735.c" "gfx.c" "ui.c" "font_data.c"
         "sd_card.c" "sd_log.c" "wav.c" "audio_proc.c" "usb_mic.c" "recorder.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        bt
        esp_http_client
        esp-tls
        mbedtls
        json
        fatfs
        sdmmc
        vfs
        usb
        usb_host_uac
)
