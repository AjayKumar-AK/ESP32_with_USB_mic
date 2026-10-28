#pragma once

#define SD_MOUNT_POINT            "/sdcard"
#define SD_SPI_FREQ_KHZ           20000
#define SPI_BUS_MAX_XFER_BYTES    16384

// ---- Mic settings carried over from the working mic firmware ---------------
#define PREFERRED_SAMPLE_RATE     16000   // format negotiation target
#define OUTPUT_RATE_HZ            16000   // decimate to this before storing
#define FIR_CUTOFF_HZ             6800.0f // anti-alias low-pass before decimation
#define DIGITAL_GAIN_X100         250     // 100 = x1.0, 250 = +8 dB

#define UAC_RING_BUF_SIZE         (32 * 1024)  // internal RAM; 64 KB did not fit next to Wi-Fi + BLE
#define UAC_RING_THRESHOLD        (8 * 1024)
#define UAC_IO_CHUNK_BYTES        (4 * 1024)   // frame-aligned per read
#define UAC_READ_TIMEOUT_MS       500

// ---- Recording pipeline -----------------------------------------------------
#define REC_RING_BYTES            (512 * 1024)   // PSRAM ring buffer
#define REC_WRITE_CHUNK_BYTES     16384          // one SD write
#define REC_FSYNC_INTERVAL_MS     1000           // crash-safety flush period
#define REC_MAX_WAV_BYTES         0xFFFFF000u    // WAV size field limit (segmenting comes later)
#define REC_NO_AUDIO_TIMEOUT_READS 10            // x UAC_READ_TIMEOUT_MS with no data -> abort

// ---- Which firmware to build ------------------------------------------------
#define APP_STAGE_DISPLAY         0   // Stage A: display demo (no mic, no SD needed)
#define APP_STAGE_MIC             1   // Step 1: USB mic -> SD test
#define APP_STAGE_PINTEST         2   // TFT wiring check with a multimeter
#define APP_STAGE_FULL            3   // the real product firmware
#define APP_STAGE_SDTEST          4   // SD card wiring/power check
#define APP_STAGE_USBTEST         5   // USB host diagnostic (any device, shows audio class)
#define APP_STAGE                 APP_STAGE_FULL

// ---- ST7735 panel (128x160, used in landscape 160x128) ---------------------
// Calibrate with the first screen of the Stage A demo, then adjust these.
#define LCD_SPI_HZ                8000000   // lowered for jumper-wire bring-up; raise to 20000000 later
#define LCD_MADCTL                0xA0   // landscape. Try 0x60 if the picture is upside down or mirrored
#define LCD_BGR                   0      // set 1 if red and blue are swapped
#define LCD_INVERT                0      // set 1 if colours look like a negative
#define LCD_XOFFSET               0      // some panels need small offsets (e.g. X 1, Y 2) - fix
#define LCD_YOFFSET               0      // until the white border on the test screen is intact

// ---- Product behaviour --------------------------------------------------------
#define FW_VERSION                "0.2"
#define TZ_STRING                 "IST-5:30"        // India Standard Time
#define NTP_SERVER_1              "pool.ntp.org"
#define NTP_SERVER_2              "time.google.com"

#define BTN_USE_BOOT_BUTTON       1       // development: the on-board BOOT button also works. Set 0 for production.
#define BTN_LONG_MS               2000    // hold this long on the normal screen for admin mode
#define BTN_DOUBLE_GAP_MS         350     // max gap between the two presses of a double press (admin mode only)

#define COUNTDOWN_SECONDS         3
#define TAP_GUARD_MS              3000    // ignore a stop-tap this soon after recording starts
#define HINT_MS                   3000    // "press button to stop current recording" banner
#define ERROR_SHOW_MS             3000
#define INFO_SHOW_MS              2000
#define ADMIN_TIMEOUT_MS          (5 * 60 * 1000)   // leave admin mode after this long without BLE activity

#define SD_MIN_START_BYTES        (50ULL * 1024 * 1024)   // refuse to start with less free space
#define SD_MIN_RUN_BYTES          (10ULL * 1024 * 1024)   // stop (and save) if free space drops below this

#define NFC_POLL_MS               120
#define NFC_RELEASE_MS            500     // a card must be out of the field this long before the next tap counts

// ---- Upload (same API and flow as the working Arduino test) -------------------------
#define API_BASE_URL              "https://coteacher.discoverlearning.in/api"
#define UPLOAD_TLS_VERIFY         1       // 0 = skip certificate checks (testing only, like setInsecure() in the Arduino test)
#define UPLOAD_CHUNK_BYTES        8192    // SD read size per network write
#define UPLOAD_MAX_ATTEMPTS       3
#define UPLOAD_WIFI_WAIT_MS       30000   // wait for Wi-Fi before giving up (file stays pending)
#define UPLOAD_TIME_WAIT_MS       30000   // wait for the NTP clock (TLS certificate dates need it)
#define UPLOAD_RETRY_INTERVAL_MS  (10 * 60 * 1000)   // pending files are retried this often while idle

// ---- SD card ----------------------------------------------------------------------------
#define SD_MOUNT_ATTEMPTS         3       // tries at boot before reporting the card as missing
#define SD_RETRY_INTERVAL_MS      10000   // while idle and unmounted, try again this often
#define SD_KEEP_FREE_BYTES        (500ULL * 1024 * 1024)   // uploaded recordings are deleted oldest-first below this
