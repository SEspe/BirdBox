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
    uint8_t  cloud_provider;        /* active SECONDARY cloud classifier
                                       (FSD §3.2.3): 0 = off (on-device only),
                                       1 = Anthropic Claude, 2 = Google Gemini.
                                       One at a time — both keys may be stored,
                                       but only the selected provider identifies
                                       new motion events, and only when its key
                                       is set. The Gallery's ✨ button works
                                       whenever any key is set, regardless. Needs
                                       WiFi and costs money per call, so default
                                       0. A failed call degrades to the on-device
                                       answer — never a dropped event
                                       (§3.2/never-drop-work). Matches
                                       cloud_provider_t in cloud.h */
    char     claude_key[128];       /* Anthropic API key ("sk-ant-..."); "" =
                                       Claude unavailable however the selector is
                                       set. Sized for ~108 chars plus headroom —
                                       these are far longer than a typical API
                                       key and a short buffer would truncate one
                                       into a silent 401. Never leaves the device
                                       except to api.anthropic.com, and is
                                       omitted from the settings export (§5) */
    char     gemini_key[128];       /* Google Gemini (AI Studio) API key; "" =
                                       Gemini unavailable however the selector is
                                       set. Same handling as claude_key: sent only
                                       to generativelanguage.googleapis.com and
                                       left out of the settings export (§5) */
    char     gemini_model[48];      /* Gemini model id for generateContent, e.g.
                                       "gemini-2.5-flash". Operator-settable
                                       because model availability shifts with
                                       account tier + generation lifecycle (a
                                       pinned id can 404 "not available to new
                                       users" or "no longer available"); the
                                       Settings Test button lists what a key can
                                       use. "" falls back to GEMINI_MODEL_DEFAULT
                                       in gemini.c. Ids are [a-z0-9.-]; validated
                                       on save so it's URL-safe */
    uint8_t  inat_cv_enabled;       /* 1 = iNaturalist online CV is the PRIMARY
                                       classifier tier (FSD §3.2.3): identify new
                                       events with iNat's score_image first,
                                       nordic model + Claude/Gemini as the
                                       fallbacks. Free (no per-call fee) but needs
                                       inat_key + WiFi; default 0. Distinct from
                                       inat_periodic_enabled (the on-device iNat
                                       batch) */
    uint8_t  ondevice_enabled;      /* 1 = the on-device model (nordic) is the
                                       SECONDARY tier in the live cascade (iNat →
                                       nordic → cloud); 0 = skip it, so events go
                                       iNat → cloud only. The model stays on the
                                       card and the manual 🔍 button still uses it;
                                       this only removes it from AUTOMATIC live
                                       classification (FSD §3.2.3). default 1 */
    char     inat_loc[24];          /* iNaturalist geo hint "lat,lng" from the
                                       capital-city dropdown (e.g. "59.91,10.75").
                                       Sent to score_image so the CV favours
                                       locally-plausible species — without it iNat
                                       runs vision-only and picks geographically
                                       absurd taxa at low confidence. "" = no geo.
                                       Validated to [0-9.,-] on save (§3.2.3) */
    char     inat_key[800];         /* iNaturalist API JWT (sent as "Bearer …").
                                       iNat JWTs EXPIRE ~24 h after issue, so this
                                       needs periodic refresh (grabbed from
                                       inaturalist.org/users/api_token). A billable-
                                       grade secret — omitted from the settings
                                       export (§5). Sized for a long JWT.
                                       Auto-refreshed from inat_session (v2.36). */
    char     inat_session[1024];    /* _inaturalist_session browser cookie (§3.2.3).
                                       Lasts weeks (vs the 24 h JWT), so the device
                                       re-fetches a fresh JWT from /users/api_token
                                       with it — no daily re-paste. A login secret,
                                       omitted from the export. Paste once. */
    char     inat_user[64];         /* iNaturalist username (or email) for the
                                       self-service login (v2.43): the box logs in
                                       like a browser (GET /login → CSRF → POST
                                       /session) to mint its OWN session cookie, so
                                       even the weeks-long cookie never needs
                                       re-pasting. "" = feature off. */
    char     inat_pass[64];         /* iNaturalist account password. A full account
                                       credential — write-only in the settings API
                                       (present-only flag in GET) and omitted from
                                       the export, but stored PLAINTEXT in NVS: a
                                       flash dump reveals it. Only set it if that
                                       trade is acceptable (§3.2.3). */
    uint8_t  tta;                   /* 1 = test-time augmentation: classify each
                                       frame plus its horizontal mirror and
                                       average the scores (FSD §3.2/v1.55) at ~2x
                                       inference time; 0 = single pass. default 0
                                       — the ~2x cost/heat bought no gain on the
                                       model's hard cases (v1.56), opt-in only */
    uint8_t  inat_periodic_enabled; /* 1 = periodically reclassify still-
                                       unclassified frames with the on-SD iNat
                                       model — whole-frame, masked to the 30
                                       target species (FSD §3.2.3, third tier).
                                       A background booster for hard cases the
                                       nordic model + Claude left unlabelled.
                                       Off by default: iNat underperforms on this
                                       domain, and each pass swaps the model
                                       (blocking live ID for the batch), so it's
                                       strictly opt-in. */
    uint16_t inat_periodic_interval_min; /* minutes between iNat batch passes
                                       when enabled; default 60, min 5. */
} settings_t;

extern settings_t g_settings;

esp_err_t settings_load(void);   /* NVS -> g_settings, defaults if absent */
esp_err_t settings_save(void);
