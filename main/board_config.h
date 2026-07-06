#pragma once

/* Per-board pin maps (FSD §2.1). Select exactly one board below, or add a new
 * block for your hardware. Camera DVP pins follow the esp32-camera component's
 * naming (PWDN/RESET/XCLK/SIOD/SIOC/Y2..Y9/VSYNC/HREF/PCLK). */

#define BOARD_XIAO_ESP32S3_SENSE   1
/* #define BOARD_FREENOVE_ESP32S3_CAM 1 */
/* #define BOARD_AITHINKER_ESP32CAM   1 */

#if defined(BOARD_XIAO_ESP32S3_SENSE)
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
#define PIN_PIR         -1     /* optional PIR (FSD §3.1); -1 = not fitted */
#define PIN_IR_LED      -1     /* optional IR illuminator (FSD §2.1) */
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
