#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "classify.h"   /* roi_t */

/* Visit-event bookkeeping (FSD §3.1, §3.4): frame saving via storage.c, the
 * per-event visit-log row, and last-event state for /api/status. The event
 * orchestration itself (when to grab which frame) lives in motion.c's task. */

/* Save one event frame to SD; on the first frame pass path_out to get the
 * web path back for the log row. `roi` is the motion region seen at (or just
 * before) this frame's grab — kept per frame so species-ID zoom follows a
 * moving bird across the event (§3.1/§3.2); roi_none() = whole frame. */
esp_err_t capture_event_frame(const uint8_t *jpeg, size_t len, roi_t roi,
                              char *path_out, size_t path_out_len);

/* Close the event: hands the saved frames (with their per-frame ROIs) to the
 * classifier (best-of-N) and updates last-event state. No-op when frames == 0. */
void capture_event_finish(int frames, int fast_count, const char *first_path);

const char *capture_last_event_path(void);   /* "" until the first event */
uint32_t    capture_event_count(void);
