/* Camera init (FSD §2.1) with the board_config.h pin map (verified against
 * the reference ESP32-S3-CAM unit by SCCB probe, FSD v1.2).
 *
 * JPEG at SVGA (800x600) — the FSD §3.3 stream default. Two frame buffers in
 * PSRAM with CAMERA_GRAB_LATEST so a slow stream client can't back the
 * sensor up; capture (§3.1) will bump resolution per-shot when implemented. */
#include "camera.h"
#include "board_config.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "camera";

static bool s_available = false;

esp_err_t camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_Y9, .pin_d6 = CAM_PIN_Y8,
        .pin_d5 = CAM_PIN_Y7, .pin_d4 = CAM_PIN_Y6,
        .pin_d3 = CAM_PIN_Y5, .pin_d2 = CAM_PIN_Y4,
        .pin_d1 = CAM_PIN_Y3, .pin_d0 = CAM_PIN_Y2,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_SVGA,
        .jpeg_quality = g_settings.stream_quality,   /* settings_load ran first */
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed (%s) — check board_config.h pin map",
                 esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "camera ready — sensor PID 0x%04x", s ? s->id.PID : 0);
    s_available = true;
    return ESP_OK;
}

bool camera_available(void) { return s_available; }

int camera_get_pid(void)
{
    if (!s_available) return 0;
    sensor_t *s = esp_camera_sensor_get();
    return s ? s->id.PID : 0;
}

esp_err_t camera_set_quality(uint8_t quality)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_quality(s, quality) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "JPEG quality set to %u", quality);
    return ESP_OK;
}
