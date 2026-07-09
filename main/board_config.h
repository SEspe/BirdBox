#pragma once

/* Per-board pin maps (FSD §2.1). Select exactly one board below, or add a new
 * block for your hardware. Camera DVP pins follow the esp32-camera component's
 * naming (PWDN/RESET/XCLK/SIOD/SIOC/Y2..Y9/VSYNC/HREF/PCLK). */

#define BOARD_ESP32S3_CAM_GENERIC  1   /* project's reference unit */
/* #define BOARD_XIAO_ESP32S3_SENSE   1 */
/* #define BOARD_AITHINKER_ESP32CAM   1 */

#if defined(BOARD_ESP32S3_CAM_GENERIC)
/* Generic "ESP32-S3-CAM" board (N16R8: 16 MB flash, 8 MB octal PSRAM,
 * OV2640). Shares the ESP32-S3-EYE / Freenove ESP32-S3-WROOM CAM camera
 * pin map — verified by SCCB probe against the project's reference unit
 * (sensor PID 0x26 = OV2640, test frame OK). */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_Y9      16
#define CAM_PIN_Y8      17
#define CAM_PIN_Y7      18
#define CAM_PIN_Y6      12
#define CAM_PIN_Y5      10
#define CAM_PIN_Y4      8
#define CAM_PIN_Y3      9
#define CAM_PIN_Y2      11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

#define PIN_STATUS_LED  2      /* TODO: verify against the physical unit — GPIO2 lit nothing when probed */
#define PIN_PIR         -1     /* optional PIR (FSD §3.1); -1 = not fitted */
#define PIN_IR_LED      48     /* onboard WS2812 illuminator (white/reddish) — confirmed
                                 * live via the Debug-tab GPIO probe (FSD v1.34/v1.35);
                                 * needs the WS2812 protocol (illum.c), not a plain level */
/* microSD: SDMMC 1-bit CLK=39 CMD=38 D0=40 (Freenove layout) — verified
 * live against the reference unit 2026-07-06 (64 GB card mounts, files
 * write and read back correctly). */
#define SD_USE_SDMMC    1
#define SD_PIN_CLK      39
#define SD_PIN_CMD      38
#define SD_PIN_D0       40
#define SD_PIN_CS       -1

#elif defined(BOARD_XIAO_ESP32S3_SENSE)
/* Seeed Studio XIAO ESP32S3 Sense — OV2640 on the Sense expansion board */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_Y9      48
#define CAM_PIN_Y8      11
#define CAM_PIN_Y7      12
#define CAM_PIN_Y6      14
#define CAM_PIN_Y5      16
#define CAM_PIN_Y4      18
#define CAM_PIN_Y3      17
#define CAM_PIN_Y2      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

#define PIN_STATUS_LED  21     /* onboard user LED */
#define PIN_PIR         -1
#define PIN_IR_LED      -1
/* microSD: XIAO Sense onboard slot, SPI mode */
#define SD_USE_SDMMC    0
#define SD_PIN_CS       21     /* NOTE: shares the LED pin on this board — verify against your revision */

#elif defined(BOARD_AITHINKER_ESP32CAM)
/* AI-Thinker ESP32-CAM (constrained target, FSD §2.1) */
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_Y9      35
#define CAM_PIN_Y8      34
#define CAM_PIN_Y7      39
#define CAM_PIN_Y6      36
#define CAM_PIN_Y5      21
#define CAM_PIN_Y4      19
#define CAM_PIN_Y3      18
#define CAM_PIN_Y2      5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

#define PIN_STATUS_LED  33     /* onboard red LED (active LOW) */
#define PIN_PIR         -1
#define PIN_IR_LED      -1
#define SD_USE_SDMMC    1      /* onboard slot is SDMMC 1-bit */
#define SD_PIN_CS       -1

#else
#error "board_config.h: no board selected"
#endif
