#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Visit-event bookkeeping (FSD §3.1, §3.4): frame saving via storage.c, the
 * per-event visit-log row, and last-event state for /api/status. The event
 * orchestration itself (when to grab which frame) lives in motion.c's task. */

/* Save one event frame to SD; on the first frame pass path_out to get the
 * web path back for the log row. */
esp_err_t capture_event_frame(const uint8_t *jpeg, size_t len,
                              char *path_out, size_t path_out_len);

/* Close the event: appends the visit-log CSV row (species "unclassified"
 * until §3.2 lands) and updates last-event state. No-op when frames == 0. */
void capture_event_finish(int frames, const char *first_path);

const char *capture_last_event_path(void);   /* "" until the first event */
uint32_t    capture_event_count(void);
