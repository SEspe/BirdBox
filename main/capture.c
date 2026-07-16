#include "capture.h"
#include "storage.h"
#include "classify.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "capture";

static char     s_last_event[96] = "";
static uint32_t s_event_count = 0;

/* Web-relative paths ("/captures/DAY/NAME.jpg") of this event's first
 * CLASSIFY_BEST_OF_N saved frames, handed to the classifier for best-of-N
 * species ID (FSD §3.2). Only the motion task touches these. */
static char  s_frame_paths[CLASSIFY_BEST_OF_N][96];
static roi_t s_frame_rois[CLASSIFY_BEST_OF_N];   /* per-frame zoom regions */
static int   s_frame_n = 0;

esp_err_t capture_event_frame(const uint8_t *jpeg, size_t len, roi_t roi,
                              char *path_out, size_t path_out_len)
{
    char path[96] = "";
    esp_err_t err = storage_save_jpeg(jpeg, len, path, sizeof(path));

    if (path_out) {                    /* first frame → a new event begins */
        s_frame_n = 0;
        if (err == ESP_OK) strlcpy(path_out, path, path_out_len);
    }
    /* Remember the first few frames' paths (and each frame's motion ROI, so
     * the zoom tracks a bird that moves between frames) so the classifier can
     * score them and keep the best; extra frames are still saved, just not
     * classified. */
    if (err == ESP_OK && s_frame_n < CLASSIFY_BEST_OF_N) {
        s_frame_rois[s_frame_n] = roi;
        strlcpy(s_frame_paths[s_frame_n++], path, sizeof(s_frame_paths[0]));
    }

    /* Record EVERY saved frame's re-detected motion box in the per-day sidecar
     * (§3.4), not just the classified first-N: a later human confirm of any
     * follow-up frame then recovers that frame's own box for ROI-crop training,
     * with no manual backfill. path is "/captures/YYYY-MM-DD/NAME.jpg". */
    if (err == ESP_OK && !roi_is_empty(roi) &&
        strncmp(path, "/captures/", 10) == 0 && strlen(path) > 21) {
        const char *fbase = strrchr(path, '/');
        char date[11];
        memcpy(date, path + 10, 10);
        date[10] = '\0';
        char roistr[24];
        snprintf(roistr, sizeof(roistr), "%.2f-%.2f-%.2f-%.2f",
                 roi.x0, roi.y0, roi.x1, roi.y1);
        storage_log_frame_roi(date, fbase + 1, roistr);
    }

    return err;
}

void capture_event_finish(int frames, const char *first_path)
{
    if (frames <= 0) {
        s_frame_n = 0;
        return;
    }

    s_event_count++;
    strlcpy(s_last_event, first_path, sizeof(s_last_event));

    char ts[24];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2020)
        strlcpy(ts, "unsynced", sizeof(ts));
    else
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_now);

    /* Hand the event's frames to the classifier, which scores up to
     * CLASSIFY_BEST_OF_N of them, keeps the most confident real species, and
     * writes the visit-log row when done (§3.2 — asynchronous, capture never
     * waits). On any failure fall back to the pre-§3.2 direct "unclassified"
     * row so no event is ever unlogged. */
    bool queued = false;
    if (s_frame_n > 0)
        queued = classify_submit_event(s_frame_paths, s_frame_rois, s_frame_n,
                                       ts, frames, first_path);
    if (!queued) {
        char line[200];
        snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,,,,", ts, frames, first_path);
        if (storage_append_visit_log(line) != ESP_OK)
            ESP_LOGW(TAG, "visit log append failed");
    }
    s_frame_n = 0;

    ESP_LOGI(TAG, "visit event #%lu: %d frame(s), first %s%s",
             (unsigned long) s_event_count, frames, first_path,
             queued ? " (classifying)" : "");
}

const char *capture_last_event_path(void) { return s_last_event; }
uint32_t    capture_event_count(void)     { return s_event_count; }
