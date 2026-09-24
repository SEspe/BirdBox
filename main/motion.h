#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Motion detection (FSD §3.1): low-res grayscale frame differencing with a
 * rolling background, sensitivity threshold + minimum-changed-area filter,
 * cool-down so one continuous visit produces one event. Runs the visit-event
 * capture pipeline (capture.c) when triggered. */

esp_err_t motion_start(void);

bool     motion_active(void);          /* an event is being captured right now */
uint32_t motion_trigger_count(void);   /* events since boot */
uint64_t motion_trigger_cells(void);   /* 8x8 mask of the last trigger's cells (§3.1) */
/* Oversized-cluster rejections (§3.1): a cluster wider than the mount-derived
 * cap is discarded as wind/foliage. Without these the discard is INVISIBLE over
 * the network — the box looks exactly like one with nothing in front of it. */
uint32_t motion_reject_count(void);    /* frames whose only cluster was too wide */
int      motion_reject_cells(void);    /* cells in the largest such cluster */
int      motion_cluster_cells(void);   /* cells in the last WINNING cluster */
int      motion_cluster_cap(void);     /* current cap, from the mount setting */

/* Seconds of post-boot detection quarantine still remaining (FSD §3.1/v1.61),
 * 0 once detection is live. Lets the UI explain why nothing triggers at boot. */
uint16_t motion_quarantine_remaining_s(void);
uint16_t motion_cooldown_remaining_s(void);   /* post-event cool-down countdown, 0 = idle (v2.57) */
uint32_t motion_fast_last_ms(void);           /* last fast-burst avg inter-frame gap, ms (v2.60) */
uint32_t motion_fast_avg_ms(void);            /* 4-event moving average of it, ms */

/* Runtime enable/disable of detection, for maintenance (FSD §5). A disabled
 * detector keeps its task alive but skips all frame grabbing/differencing, so
 * the live stream and manual snapshots still work while no visit events fire.
 * Not persisted: detection always comes up enabled after a reboot. */
bool motion_detection_enabled(void);
void motion_set_detection_enabled(bool enabled);

/* ── Ambient light (FSD §14) ────────────────────────────────────────────────
 * The debounced dark/bright state the illuminator and fast-shutter already
 * follow, plus a one-shot measurement for the night sleep probe. The probe is
 * needed because a paused detector grabs no frames at all: disabling detection
 * (or sleeping the camera) blinds the very sensor that would otherwise notice
 * dawn, so waking has to be driven by a deliberate, timed sample rather than
 * by the continuous reading. Returns average luma 0-255, or -1. */
bool motion_ambient_dark(void);
int  motion_ambient_probe(void);

/* Night-sleep pause (FSD §14), deliberately SEPARATE from the maintenance
 * toggle above. Detection runs only when enabled and not night-paused, so the
 * two reasons cannot overwrite each other — sharing one flag produced a box
 * stuck with detection off while night sleep thought it was awake. Unlike the
 * maintenance pause, a night pause keeps sampling ambient light whenever the
 * sensor is powered, so pausing can never blind the reading that ends it. */
bool motion_night_paused(void);
void motion_set_night_paused(bool paused);

/* Contrast (std) and peak luma of the last decoded frame — the values that
 * decide darkness since v3.12. Mean brightness is only a display number: AGC
 * holds it near 140 as the light fails, so an unusable dusk frame measured a
 * HIGHER mean than noon. Exposed so "dark" is explainable rather than magic. */
int motion_ambient_contrast(void);
int motion_ambient_peak(void);
