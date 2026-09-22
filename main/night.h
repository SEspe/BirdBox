#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Night sleep (FSD §14) — stop working after dark, on the box's own eyes.
 *
 * There are no birds at night, so detection, capture, classification, the iNat
 * calls and the illuminator are all waste, along with the heat they make. The
 * trigger is the ambient-light reading motion.c already computes from every
 * detect frame, so no almanac, no location and no clock are required: a shaded
 * or north-facing site self-corrects, which an almanac would get wrong.
 *
 * THE ASYMMETRY IS THE WHOLE DESIGN. Going to sleep is driven by a continuous,
 * free measurement. Waking up cannot be, because pausing detection stops the
 * frame grabs that produce that measurement (motion.c's loop explicitly does
 * not touch the camera while disabled) and sleeping the sensor makes it
 * absolute — the box has switched off the very eye that would see dawn. So
 * waking is a deliberate timed PROBE: power the camera, discard the AEC
 * warm-up frames, take one reading, and either resume or go back to sleep. */

typedef enum {
    NIGHT_OFF         = 0,   /* always online (default) */
    NIGHT_STOP_DETECT = 1,   /* pause detection + power the sensor down; the
                                web UI, OTA and Home Assistant stay up */
    NIGHT_DEEP_SLEEP  = 2,   /* ESP32 deep sleep between probes; the box leaves
                                the LAN entirely between wake windows */
} night_mode_t;

esp_err_t night_start(void);

/* Live state for /api/night and the Settings tab. */
bool        night_asleep(void);
const char *night_state_str(void);   /* "online" | "sleeping" | "probing" | "disabled" */
int         night_last_luma(void);   /* last probe's average luma 0-255, -1 if none */
int         night_asleep_s(void);    /* seconds asleep, 0 when awake */

/* Manual override: wake now and hold off re-sleeping for NIGHT_SNOOZE_MIN, so
 * someone inspecting the box after dark is not put straight back to sleep. */
void night_wake_now(void);

/* Why an enabled box is still awake: "" (nothing holding it), "bright",
 * "snoozed", "live viewer" or "ota in progress". Without this, an enabled box
 * that never sleeps is indistinguishable from a broken one — the same silent
 * state the motion cluster-cap telemetry exists to expose. */
const char *night_hold_reason(void);
