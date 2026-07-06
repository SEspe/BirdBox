#pragma once
#include "esp_err.h"

/* Motion detection (FSD §3.1): low-res grayscale frame differencing with
 * rolling background; optional PIR pre-trigger; debounce/cool-down so one
 * visit = one event. Fires capture_visit_event() on trigger. */

esp_err_t motion_start(void);
