#include "capture.h"
#include "esp_log.h"

static const char *TAG = "capture";

esp_err_t capture_visit_event(void)
{
    /* TODO(FSD §3.1, §3.4): capture sequence -> storage_write_capture(),
     * visit-log append, classify_queue_frame(). Retention pruning at the
     * configured SD cap (default 80 %), favorites exempt. All SD writes go
     * through storage.c's single writer task (FSD §7). */
    ESP_LOGW(TAG, "capture_visit_event: not implemented (scaffold)");
    return ESP_OK;
}
