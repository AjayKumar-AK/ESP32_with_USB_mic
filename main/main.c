// One codebase, built up in stages. Pick the stage in app_config.h (APP_STAGE).
#include "app_config.h"
#include "stages.h"

void app_main(void)
{
#if APP_STAGE == APP_STAGE_FULL
    stage_full_app();
#elif APP_STAGE == APP_STAGE_USBTEST
    stage_usbtest();
#elif APP_STAGE == APP_STAGE_SDTEST
    stage_sdtest();
#elif APP_STAGE == APP_STAGE_DISPLAY
    stage_display_demo();
#elif APP_STAGE == APP_STAGE_PINTEST
    stage_pintest();
#else
    stage_mic_test();
#endif
}
