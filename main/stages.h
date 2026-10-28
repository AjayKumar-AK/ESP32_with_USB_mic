#pragma once
void stage_display_demo(void);   // Stage A: ST7735 screens with fake data
void stage_mic_test(void);       // Step 1: USB mic -> SD (BOOT button start/stop)
void stage_pintest(void);        // wiring check: toggles each TFT line slowly for a multimeter
void stage_full_app(void);        // the real flow: NFC, recording, admin mode, Wi-Fi/time
void stage_sdtest(void);         // SD wiring check: bit-banged CMD0/CMD8, prints what the card says
void stage_usbtest(void);        // USB host diagnostic: lists every enumerated device and its audio class
