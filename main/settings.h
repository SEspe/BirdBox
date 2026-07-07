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
    char     timezone[48];          /* default "Europe/Oslo" posix TZ */
    char     region[32];            /* species-model region: filename under
                                       /sd/model (FSD §3.2), "" = auto */
    char     ntp_server[64];        /* SNTP hostname (FSD §3.4), default
                                       "pool.ntp.org" */
    species_lang_t lang;            /* species display language (FSD §3.2),
                                       default LANG_EN; scientific name is
                                       always shown alongside it */
} settings_t;

extern settings_t g_settings;

esp_err_t settings_load(void);   /* NVS -> g_settings, defaults if absent */
esp_err_t settings_save(void);
