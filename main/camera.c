/* TEMPORARY board-identification probe (this session): tries known
 * ESP32-S3 camera pin maps until a sensor answers on SCCB, then grabs one
 * test frame. Will be replaced by the real camera_init (FSD §2.1) once the
 * board is identified and pinned in board_config.h. */
#include "camera.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "camera";

typedef struct {
    const char *name;
    int pwdn, reset, xclk, siod, sioc;
    int y9, y8, y7, y6, y5, y4, y3, y2;
    int vsync, href, pclk;
} pinmap_t;

static const pinmap_t k_maps[] = {
    { "ESP32-S3-EYE / Freenove S3 CAM / generic S3-CAM",
      -1, -1, 15, 4, 5, 16, 17, 18, 12, 10, 8, 9, 11, 6, 7, 13 },
    { "Seeed XIAO ESP32S3 Sense",
      -1, -1, 10, 40, 39, 48, 11, 12, 14, 16, 18, 17, 15, 38, 47, 13 },
    { "LilyGO T-Camera S3",
      -1, 39, 38, 5, 4, 9, 10, 11, 13, 21, 48, 47, 14, 8, 18, 12 },
};

esp_err_t camera_init(void)
{
    for (unsigned i = 0; i < sizeof(k_maps) / sizeof(k_maps[0]); i++) {
        const pinmap_t *m = &k_maps[i];
        camera_config_t c = {
            .pin_pwdn = m->pwdn, .pin_reset = m->reset, .pin_xclk = m->xclk,
            .pin_sccb_sda = m->siod, .pin_sccb_scl = m->sioc,
            .pin_d7 = m->y9, .pin_d6 = m->y8, .pin_d5 = m->y7, .pin_d4 = m->y6,
            .pin_d3 = m->y5, .pin_d2 = m->y4, .pin_d1 = m->y3, .pin_d0 = m->y2,
            .pin_vsync = m->vsync, .pin_href = m->href, .pin_pclk = m->pclk,
            .xclk_freq_hz = 20000000,
            .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
            .pixel_format = PIXFORMAT_JPEG,
            .frame_size = FRAMESIZE_SVGA,
            .jpeg_quality = 12,
            .fb_count = 1,
            .fb_location = CAMERA_FB_IN_PSRAM,
            .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        };
        ESP_LOGI(TAG, "probing pin map %u: %s", i, m->name);
        esp_err_t err = esp_camera_init(&c);
        if (err == ESP_OK) {
            sensor_t *s = esp_camera_sensor_get();
            ESP_LOGI(TAG, "*** MATCH: %s — sensor PID 0x%04x ***",
                     m->name, s ? s->id.PID : 0);
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                ESP_LOGI(TAG, "test frame OK: %ux%u, %u bytes",
                         fb->width, fb->height, (unsigned) fb->len);
                esp_camera_fb_return(fb);
            } else {
                ESP_LOGW(TAG, "sensor found but no frame delivered");
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "no camera on this map (%s)", esp_err_to_name(err));
        esp_camera_deinit();
    }
    ESP_LOGE(TAG, "no known pin map matched — board needs manual identification");
    return ESP_OK;   /* don't abort boot over a probe failure */
}
