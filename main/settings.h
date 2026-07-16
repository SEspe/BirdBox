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
    uint16_t cooldown_s;            /* default 3 */
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
                                       change needs a reboot. default = HD (1280x720) */
    int8_t   contrast;              /* OV2640 contrast -2..+2 (the sensor has no
                                       sharpness control); applied live. default 0 */
    int8_t   ae_level;              /* OV2640 auto-exposure level -2..+2: shifts the
                                       AE target brighter/darker for scenes the
                                       default metering renders too dark/bright.
                                       Applied live. default 0 */
    char     timezone[48];          /* default "Europe/Oslo" posix TZ */
    char     region[32];            /* species-model region: filename under
                                       /sd/model (FSD §3.2), "" = auto */
    char     ntp_server[64];        /* SNTP hostname (FSD §3.4), default
                                       "pool.ntp.org" */
    char     stats_reset_ts[20];    /* "YYYY-MM-DDTHH:MM:SS" epoch of the last
                                       stats reset; stats count only rows at/after
                                       it (FSD §3.4). "" = count all. Non-
                                       destructive: the visit log is never deleted,
                                       so labels/ROIs (gallery + training) persist */
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
    uint16_t detect_quarantine_s;   /* after boot, suppress motion detection for
                                       this many seconds so the camera's warm-up
                                       frames and the pre-SNTP (~1970) clock — which
                                       files captures under /no-date — can't fire
                                       false events during startup (FSD §3.1/v1.61).
                                       default 60; 0 = disabled */
    uint8_t  claude_enabled;        /* 1 = identify new motion events with the
                                       Anthropic Claude API INSTEAD of the
                                       on-device TFLM model (FSD §3.2.3); the
                                       Gallery's Claude button works whenever a
                                       key is set, regardless. Needs WiFi and
                                       costs money per call, so default 0. A
                                       failed call degrades to the on-device
                                       answer — never to a dropped event
                                       (§3.2/never-drop-work) */
    char     claude_key[128];       /* Anthropic API key ("sk-ant-..."); "" =
                                       Claude unavailable however the toggle is
                                       set. Sized for ~108 chars plus headroom —
                                       these are far longer than a typical API
                                       key and a short buffer would truncate one
                                       into a silent 401. Never leaves the device
                                       except to api.anthropic.com, and is
                                       omitted from the settings export (§5) */
    uint8_t  tta;                   /* 1 = test-time augmentation: classify each
                                       frame plus its horizontal mirror and
                                       average the scores (FSD §3.2/v1.55) at ~2x
                                       inference time; 0 = single pass. default 0
                                       — the ~2x cost/heat bought no gain on the
                                       model's hard cases (v1.56), opt-in only */
} settings_t;

extern settings_t g_settings;

esp_err_t settings_load(void);   /* NVS -> g_settings, defaults if absent */
esp_err_t settings_save(void);
