#include "capture.h"
#include "storage.h"
#include "classify.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "capture";

static char     s_last_event[96] = "";
static uint32_t s_event_count = 0;

/* PSRAM copy of the event's best (first) frame, kept for the classifier
 * (FSD §3.2) — the original buffer goes back to the camera driver right
 * after the SD write. Only the motion task touches this. */
static uint8_t *s_best_jpeg = NULL;
static size_t   s_best_len  = 0;

esp_err_t capture_event_frame(const uint8_t *jpeg, size_t len,
                              char *path_out, size_t path_out_len)
{
    esp_err_t err = storage_save_jpeg(jpeg, len, path_out, path_out_len);

    if (path_out && err == ESP_OK && classify_available()) {   /* first frame */
        free(s_best_jpeg);
        s_best_jpeg = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_best_jpeg) {
            memcpy(s_best_jpeg, jpeg, len);
            s_best_len = len;
        }
    }
    return err;
}

void capture_event_finish(int frames, const char *first_path)
{
    if (frames <= 0) {
        free(s_best_jpeg);
        s_best_jpeg = NULL;
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

    /* Hand the best frame to the classifier, which writes the visit-log row
     * with the real species + confidence when inference completes (§3.2 —
     * asynchronous, capture never waits). On any failure fall back to the
     * pre-§3.2 direct "unclassified" row so no event is ever unlogged. */
    bool queued = false;
    if (s_best_jpeg)
        queued = classify_submit_event(s_best_jpeg, s_best_len, ts,
                                       frames, first_path);
    if (!queued) {
        free(s_best_jpeg);
        char line[200];
        snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,,", ts, frames, first_path);
        if (storage_append_visit_log(line) != ESP_OK)
            ESP_LOGW(TAG, "visit log append failed");
    }
    s_best_jpeg = NULL;
    s_best_len  = 0;

    ESP_LOGI(TAG, "visit event #%lu: %d frame(s), first %s%s",
             (unsigned long) s_event_count, frames, first_path,
             queued ? " (classifying)" : "");
}

const char *capture_last_event_path(void) { return s_last_event; }
uint32_t    capture_event_count(void)     { return s_event_count; }
