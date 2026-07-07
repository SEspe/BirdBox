#include "classify.h"
#include "esp_log.h"

static const char *TAG = "classify";

esp_err_t classify_init(void)
{
    /* TODO(FSD §3.2): load regional model from /model/ on SD (fallback:
     * flash-embedded), inference task consuming a frame queue, attach
     * top-3 + confidence to the visit event when done (< 5 s target).
     * Confidence threshold from settings (default 60 %). */
#if !CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGW(TAG, "species ID unavailable on this target (FSD §3.2) — captures will be 'Unclassified'");
#endif
    ESP_LOGW(TAG, "classify_init: not implemented (scaffold)");
    return ESP_OK;
}

int32_t classify_last_duration_ms(void) { return -1; }
