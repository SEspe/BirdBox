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

/* Seconds of post-boot detection quarantine still remaining (FSD §3.1/v1.61),
 * 0 once detection is live. Lets the UI explain why nothing triggers at boot. */
uint16_t motion_quarantine_remaining_s(void);

/* Runtime enable/disable of detection, for maintenance (FSD §5). A disabled
 * detector keeps its task alive but skips all frame grabbing/differencing, so
 * the live stream and manual snapshots still work while no visit events fire.
 * Not persisted: detection always comes up enabled after a reboot. */
bool motion_detection_enabled(void);
void motion_set_detection_enabled(bool enabled);
