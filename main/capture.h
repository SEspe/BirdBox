#pragma once
#include "esp_err.h"

/* Visit-event capture (FSD §3.1): on motion trigger, grab 1 full-res JPEG +
 * up to N follow-ups, write to /captures/YYYY-MM-DD/ on SD, append a row to
 * the visit log (FSD §3.4), then queue the best frame for classification. */

esp_err_t capture_visit_event(void);
