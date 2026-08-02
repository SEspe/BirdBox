#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Runtime settings, persisted in NVS (FSD §5 Settings tab). */

typedef enum { MODE_NESTBOX = 0, MODE_FEEDER = 1 } placement_mode_t;
typedef enum { LANG_EN = 0, LANG_NO = 1 } species_lang_t;
/* Legacy quarter-turn rotation. Kept ONLY so the v2.83 NVS migration can read
 * the old `s_rot` values (0-3) it is stored as on every deployed box; nothing
 * new should use it. Superseded by settings_t.rot_deg, which is free degrees. */
typedef enum { ROTATE_0 = 0, ROTATE_90 = 1, ROTATE_180 = 2, ROTATE_270 = 3 } rotation_t;
/* Autofocus mode (FSD §5), OV5640-class sensors with a VCM lens only. OFF is
 * the default and also what every fixed-focus module gets: it skips the AF
 * firmware download entirely, so an OV2640 box pays nothing for this existing. */
typedef enum { FOCUS_OFF = 0, FOCUS_AUTO = 1, FOCUS_MANUAL = 2 } focus_mode_t;

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
    /* ── View transform (FSD §5, v2.83) ────────────────────────────────────
     * Mount correction for a camera that isn't level or is facing the "wrong"
     * way. This is a DISPLAY transform: the live view and the image grids are
     * corrected in the browser, and the JPEGs written to the SD card are left
     * exactly as the sensor produced them (operator's call — a non-quarter
     * angle cannot be applied to a JPEG without resampling it, and quarter
     * turns were not worth a re-encode either).
     *
     * The single exception is EXACTLY 180 deg, which the sensor does for free
     * and losslessly via hmirror+vflip. That predates this field and stored
     * captures already depend on it, so it stays at the sensor and the display
     * must NOT rotate again on top (see camera_set_rotation / applyView). */
    uint16_t rot_deg;               /* 0-359, any angle. default 0. Migrated
                                       once from the old 0-3 quarter-turn enum
                                       (s_rot) into the new NVS key s_rotd —
                                       a bare 0-3 is ambiguous between the two
                                       meanings, hence the separate key */
    uint8_t  mirror_h;              /* 1 = flip the view left-right, display-side.
                                       Corrects a camera shooting through a
                                       mirror/prism or mounted facing back at
                                       itself. default 0 */
    uint8_t  mirror_v;              /* 1 = flip the view top-bottom, display-side.
                                       default 0 */
    uint8_t  region_filter;         /* 0 = iNat's global result list as-is, 1 =
                                       restrict IDs to the Norway allowlist
                                       (species_in_region, 147 names — FSD §3.2.1),
                                       default 1 */
    uint8_t  resolution;            /* camera frame-size index into camera.c's
                                       RES table; applied at camera_init, so a
                                       change needs a reboot. default = HD (1280x720) */
    int8_t   contrast;              /* sensor contrast -2..+2; on the OV2640, which
                                       has no sharpness control, this is the only
                                       "crispness" knob. Applied live. default 0 */
    int8_t   ae_level;              /* auto-exposure level -2..+2: shifts the
                                       AE target brighter/darker for scenes the
                                       default metering renders too dark/bright.
                                       Applied live. default 0 */
    int8_t   sharpness;             /* OV5640-class edge sharpening -3..+3, applied
                                       live (FSD §5). Ignored on a sensor without
                                       one (the OV2640's setter is a -1 stub) —
                                       the Settings UI hides the control there
                                       rather than offering a dead knob. default 0 */
    uint8_t  denoise;               /* OV5640-class denoise 0..8, 0 = off. Same
                                       sensor-gating as sharpness. Worth raising
                                       only in poor light, where it trades feather
                                       detail for less chroma noise — which is the
                                       wrong trade for species ID, hence default 0 */
    uint8_t  focus_mode;            /* focus_mode_t: 0 off (default), 1 continuous
                                       autofocus, 2 manual at focus_pos. Non-OFF
                                       costs a ~1 s AF firmware download at each
                                       camera init and only does anything on an
                                       OV5640 module with a VCM lens (FSD §5) */
    uint16_t focus_pos;             /* manual focus position 0..1023, near→far;
                                       only read when focus_mode == FOCUS_MANUAL.
                                       default 0 */
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
    species_lang_t lang;            /* UI + species display language (FSD §3.2,
                                       §5/v2.72), default LANG_NO; the scientific
                                       name is always shown alongside it */
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
