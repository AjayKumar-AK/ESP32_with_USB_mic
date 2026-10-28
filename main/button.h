#pragma once
#include <stdbool.h>

// Posts EVT_BTN_SINGLE / EVT_BTN_DOUBLE / EVT_BTN_LONG to the app queue.
void button_init(void);
// Double-press detection delays a single press by BTN_DOUBLE_GAP_MS, so it is only
// enabled in admin mode. When off, a short press fires instantly as SINGLE.
void button_set_double_enabled(bool on);
