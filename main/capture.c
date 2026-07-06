#include "capture.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "capture";

static char     s_last_event[96] = "";
static uint32_t s_event_count = 0;

esp_err_t capture_event_frame(const uint8_t *jpeg, size_t len,
                              char *path_out, size_t path_out_len)
{
    return storage_save_jpeg(jpeg, len, path_out, path_out_len);
}

void capture_event_finish(int frames, const char *first_path)
{
    if (frames <= 0) return;

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

    /* Columns per FSD §3.4; species/confidence stay placeholders until §3.2,
     * corrected empty until the gallery ships label correction. */
    char line[200];
    snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,", ts, frames, first_path);
    if (storage_append_visit_log(line) != ESP_OK)
        ESP_LOGW(TAG, "visit log append failed");

    /* TODO(FSD §3.2): queue first_path's frame for species classification */

    ESP_LOGI(TAG, "visit event #%lu: %d frame(s), first %s",
             (unsigned long) s_event_count, frames, first_path);
}

const char *capture_last_event_path(void) { return s_last_event; }
uint32_t    capture_event_count(void)     { return s_event_count; }
