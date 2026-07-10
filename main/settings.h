#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Runtime settings, persisted in NVS (FSD §5 Settings tab). */

typedef enum { MODE_NESTBOX = 0, MODE_FEEDER = 1 } placement_mode_t;
typedef enum { LANG_EN = 0, LANG_NO = 1 } species_lang_t;
/* Mount-correction rotation (FSD §5). 0/180 are applied at the sensor
 * (OV2640 hmirror+vflip, free); 90/270 aren't supported in OV2640 hardware,
 * so those are corrected in software per-consumer instead (see camera.c and
 * classify.cpp). */
typedef enum { ROTATE_0 = 0, ROTATE_90 = 1, ROTATE_180 = 2, ROTATE_270 = 3 } rotation_t;

typedef struct {
    placement_mode_t mode;
    uint8_t  motion_sensitivity;    /* 0-100 */
    uint8_t  capture_count;         /* follow-up frames per event, default 5 */
    uint16_t capture_interval_ms;
    uint16_t cooldown_s;            /* default 10 */
    uint8_t  confidence_pct;        /* species-ID threshold, default 60 */
    uint8_t  sd_cap_pct;            /* retention cap, default 80 */
    uint8_t  stream_quality;        /* sensor JPEG quality, lower = better */
    uint8_t  ir_led_mode;           /* 0 off, 1 auto */
    rotation_t rotation;            /* mount-correction rotation, default ROTATE_0 */
    uint8_t  region_filter;         /* 0 = global model as-is, 1 = restrict IDs to
                                       the Northern-European species set (FSD §3.2.1),
                                       default 0 */
    uint8_t  resolution;            /* camera frame-size index into camera.c's
                                       RES table; applied at camera_init, so a
                                       change needs a reboot. default = SVGA */
    int8_t   contrast;              /* OV2640 contrast -2..+2 (the sensor has no
                                       sharpness control); applied live. default 0 */
    char     timezone[48];          /* default "Europe/Oslo" posix TZ */
    char     region[32];            /* species-model region: filename under
                                       /sd/model (FSD §3.2), "" = auto */
    char     ntp_server[64];        /* SNTP hostname (FSD §3.4), default
                                       "pool.ntp.org" */
    species_lang_t lang;            /* species display language (FSD §3.2),
                                       default LANG_EN; scientific name is
                                       always shown alongside it */
    uint64_t detect_zone;           /* 8x8 detection-zone mask (FSD §3.1), bit
                                       (row*8+col), row 0 = top. A set bit means
                                       that cell counts toward motion; cleared
                                       cells are ignored (mask off a swaying
                                       branch / busy background). default all-on
                                       (~0) = whole frame, unchanged behaviour */
    uint8_t  detect_zoom;           /* 1 = crop species-ID input to the changed
                                       cells' bounding box so the bird fills the
                                       model input (FSD §3.2), 0 = center-crop the
                                       whole frame as before. default 1 */
    uint8_t  fast_shutter;          /* 1 = fixed short exposure + auto gain, to
                                       cut motion blur on a close/fast-moving
                                       bird at the cost of a noisier/darker
                                       image (FSD §2.1); 0 = normal auto
                                       exposure. default 0 */
    uint8_t  tta;                   /* 1 = test-time augmentation: classify each
                                       frame plus its horizontal mirror and
                                       average the scores, lifting confidence on
                                       hard poses (FSD §3.2/v1.55) at ~2x
                                       inference time; 0 = single pass. default 1 */
} settings_t;

extern settings_t g_settings;

esp_err_t settings_load(void);   /* NVS -> g_settings, defaults if absent */
esp_err_t settings_save(void);
