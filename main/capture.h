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

/* How long the classifier handoff blocked the motion task on the last event,
 * and the worst since boot. classify_submit_event() waits up to 15 s for a
 * queue slot and runs in the motion task, so this is detection downtime. */
int32_t     capture_submit_ms(void);
int32_t     capture_submit_max_ms(void);

const char *capture_last_event_path(void);   /* "" until the first event */
uint32_t    capture_event_count(void);
int         capture_last_frames(void);        /* total frames saved for the most recent event */
int         capture_last_fast(void);          /* of those, how many were fast-burst (v2.59) */
