#pragma once
#include <stdbool.h>
#include <stdint.h>

// The product state machine (normal / countdown / recording / saving / admin / notice).
// Never returns. Modules must be initialised first (app_events_init() before any of them).
void app_ctrl_run(uint32_t boot_count, bool have_display);
