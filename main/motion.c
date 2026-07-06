#include "motion.h"
#include "esp_log.h"

static const char *TAG = "motion";

esp_err_t motion_start(void)
{
    /* TODO(FSD §3.1): detection task — frame diff against rolling
     * background, sensitivity threshold + minimum-changed-area filter,
     * optional PIR first stage (PIN_PIR), cool-down (default 10 s).
     * Must keep running while /stream clients are active (FSD §3.3). */
    ESP_LOGW(TAG, "motion_start: not implemented (scaffold)");
    return ESP_OK;
}
