#include "settings.h"
#include "version.h"
#include "esp_log.h"

static const char *TAG = "settings";

settings_t g_settings = {
    .mode              = MODE_FEEDER,
    .motion_sensitivity = 50,
    .capture_count      = 5,
    .capture_interval_ms = 1000,
    .cooldown_s         = 10,
    .confidence_pct     = 60,
    .sd_cap_pct         = 80,
    .stream_quality     = 12,
    .ir_led_mode        = 0,
    .timezone           = "CET-1CEST,M3.5.0,M10.5.0/3",
};

esp_err_t settings_load(void)
{
    /* TODO(FSD §5): nvs_get_blob in NVS_NAMESPACE; keep defaults on absence. */
    ESP_LOGW(TAG, "settings_load: not implemented (scaffold) — using defaults");
    return ESP_OK;
}

esp_err_t settings_save(void)
{
    /* TODO(FSD §5): nvs_set_blob + commit. */
    ESP_LOGW(TAG, "settings_save: not implemented (scaffold)");
    return ESP_OK;
}
