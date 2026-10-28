#pragma once
#include <stdbool.h>

typedef enum {
    EVT_TAP,              // card tapped (uid)
    EVT_BTN_SINGLE,
    EVT_BTN_DOUBLE,
    EVT_BTN_LONG,
    EVT_ADMIN_EXIT,       // admin script asked to leave admin mode
    EVT_CONFIG_CHANGED,   // class/section changed over BLE
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    char uid[24];
} app_event_t;

void app_events_init(void);
bool app_post_event(const app_event_t *e);

static inline bool app_post(app_evt_type_t t)
{
    app_event_t e = { .type = t };
    return app_post_event(&e);
}
