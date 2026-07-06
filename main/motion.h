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
