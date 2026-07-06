#include "camera.h"
#include "board_config.h"
#include "esp_log.h"

static const char *TAG = "camera";

esp_err_t camera_init(void)
{
    /* TODO(FSD §2.1): esp_camera_init() with board_config.h pins.
     * Full-res JPEG for captures, low-res grayscale path for motion
     * detection (FSD §3.1), stream resolution independent of capture
     * resolution (FSD §3.3, default 800x600). Frame buffers in PSRAM. */
    ESP_LOGW(TAG, "camera_init: not implemented (scaffold)");
    return ESP_OK;
}
