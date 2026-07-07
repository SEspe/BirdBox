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

/* Selectable frame sizes (Settings → resolution, FSD §5). The index is what's
 * stored in g_settings.resolution; higher = more detail on small/distant birds
 * at the cost of PSRAM + inference time. Applied at camera_init, so a change
 * needs a reboot (buffers are sized here). SXGA is the ceiling: UXGA fits the
 * camera but leaves no PSRAM for the classifier's runtime JPEG-decode buffer,
 * so inference fails at UXGA — verified live (FSD §5). A stale out-of-range
 * value (e.g. an old UXGA=5) falls back to SVGA via res_idx(). */
static const struct { framesize_t fs; const char *str; } RES[] = {
    { FRAMESIZE_VGA,  "VGA 640x480"    },
    { FRAMESIZE_SVGA, "SVGA 800x600"   },
    { FRAMESIZE_XGA,  "XGA 1024x768"   },
    { FRAMESIZE_HD,   "HD 1280x720"    },
    { FRAMESIZE_SXGA, "SXGA 1280x1024" },
};
#define RES_COUNT (sizeof(RES) / sizeof(RES[0]))

static uint8_t res_idx(void)
{
    return g_settings.resolution < RES_COUNT ? g_settings.resolution : 1; /* SVGA */
}

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
        .frame_size   = RES[res_idx()].fs,           /* settings_load ran first */
        .jpeg_quality = g_settings.stream_quality,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    /* A high resolution can exhaust PSRAM alongside the species model/arena
     * (classify_init runs first, in main.c, so the model is already reserved).
     * Step down through the RES table until a size fits, rather than failing
     * boot (which would trigger OTA rollback). g_settings.resolution is updated
     * in RAM to what actually initialized, so the UI reflects reality. */
    uint8_t idx = res_idx();
    esp_err_t err = ESP_FAIL;
    while (1) {
        cfg.frame_size = RES[idx].fs;
        err = esp_camera_init(&cfg);
        if (err == ESP_OK) break;
        esp_camera_deinit();
        if (idx == 0) break;
        ESP_LOGW(TAG, "camera init at %s failed (%s) — trying a smaller size",
                 RES[idx].str, esp_err_to_name(err));
        idx--;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed (%s) — check board_config.h pin map",
                 esp_err_to_name(err));
        return err;
    }
    g_settings.resolution = idx;

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "camera ready — sensor PID 0x%04x, %s", s ? s->id.PID : 0,
             RES[idx].str);
    s_available = true;
    camera_set_rotation(g_settings.rotation);   /* settings_load ran first */
    camera_set_contrast(g_settings.contrast);
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

esp_err_t camera_set_contrast(int level)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    if (level < -2) level = -2;
    if (level >  2) level =  2;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_contrast(s, level) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "contrast set to %d", level);
    return ESP_OK;
}

const char *camera_framesize_str(void) { return RES[res_idx()].str; }

esp_err_t camera_set_rotation(rotation_t rot)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_FAIL;
    bool flip180 = (rot == ROTATE_180);
    s->set_hmirror(s, flip180);
    s->set_vflip(s, flip180);
    ESP_LOGI(TAG, "rotation %u deg requested; sensor set to %u deg (90/270 handled in software)",
             rot * 90, flip180 ? 180 : 0);
    return ESP_OK;
}
