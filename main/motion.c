/* Motion detection + visit-event orchestration (FSD §3.1).
 *
 * The sensor delivers JPEG (needed for stream/captures), so detection frames
 * are made by decoding each JPEG at 1/8 scale (SVGA → 100×75) with the
 * esp_jpeg component and reducing RGB565 to grayscale. ~7.5 kpx per compare
 * at ~4 Hz is cheap, and needs no sensor reconfiguration.
 *
 * Rolling background: exponential moving average (7/8 old + 1/8 new),
 * updated only while no motion is seen — so a bird sitting still doesn't
 * get absorbed into the background mid-visit. The baseline is re-seeded
 * after each event's cool-down (light may have changed during the visit).
 *
 * Event pipeline (one task, which also serializes all its SD writes):
 * trigger → save first frame immediately → up to capture_count-1 follow-ups
 * at capture_interval_ms while motion persists → visit-log row → cool-down. */
#include "motion.h"
#include "camera.h"
#include "capture.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "jpeg_decoder.h"

static const char *TAG = "motion";

/* 1/8-scale detection frame must fit the largest selectable resolution, which
 * is the SXGA ceiling (1280x1024 -> 160x128); HD (1280x720 -> 160x90) fits too.
 * These were sized for the old XGA ceiling (128x96); once SXGA/HD were added in
 * v1.21 an SXGA frame (160x128 = 20480 px) overran DETECT_MAX_PX and every
 * detect frame was rejected — motion detection silently died at high res. */
#define DETECT_MAX_W      160            /* 1/8 of SXGA width  (1280) */
#define DETECT_MAX_H      128            /* 1/8 of SXGA height (1024) */
#define DETECT_MAX_PX     (DETECT_MAX_W * DETECT_MAX_H)
#define DETECT_PERIOD_MS  250
#define PIX_DIFF_THR      25             /* per-pixel gray delta that counts as changed */

static uint8_t *s_bg, *s_cur, *s_rgb;
static bool     s_have_bg = false;
static int      s_px = 0;
static int      s_w = 0, s_h = 0;        /* current detect-frame dimensions */
static roi_t    s_roi;                   /* changed-cell bbox of the last trigger */

static volatile bool     s_motion_active = false;
static volatile uint32_t s_trigger_count = 0;
static volatile bool     s_detect_enabled = true;   /* default on at boot (FSD §5) */

#define GRID_N 8                         /* 8x8 detection grid (FSD §3.1) */

/* Grab one frame, decode small, update s_cur; returns true when the changed
 * area exceeds the sensitivity-derived threshold. Rolls the background only
 * on no-motion frames. */
static bool detect_once(void)
{
    camera_fb_t *fb = camera_grab();
    if (!fb) return false;

    esp_jpeg_image_cfg_t jcfg = {
        .indata      = fb->buf,
        .indata_size = fb->len,
        .outbuf      = s_rgb,
        .outbuf_size = DETECT_MAX_PX * 2,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_1_8,
    };
    esp_jpeg_image_output_t out = {0};
    esp_err_t err = esp_jpeg_decode(&jcfg, &out);
    camera_return(fb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "detect decode failed: %s", esp_err_to_name(err));
        return false;
    }

    int px = out.width * out.height;
    if (px <= 0 || px > DETECT_MAX_PX) return false;
    if (px != s_px) { s_px = px; s_have_bg = false; }
    s_w = out.width;
    s_h = out.height;

    /* Grayscale ≈ green channel of RGB565 (6 bits, scaled to 8) — a stable
     * transform is all differencing needs, not colorimetric accuracy. */
    const uint16_t *rgb = (const uint16_t *) s_rgb;
    for (int i = 0; i < px; i++)
        s_cur[i] = (uint8_t) (((rgb[i] >> 5) & 0x3F) << 2);

    if (!s_have_bg) {
        memcpy(s_bg, s_cur, px);
        s_have_bg = true;
        return false;
    }

    /* Per-cell changed-pixel tallies over the 8x8 grid. The zone mask (§3.1)
     * decides which cells count toward motion; changed cells inside the zone
     * form the ROI handed to species ID for zoom (§3.2). */
    int cell_changed[GRID_N * GRID_N] = {0};
    int cell_total[GRID_N * GRID_N]   = {0};
    const int W = s_w, H = s_h;
    for (int y = 0; y < H; y++) {
        int cy = y * GRID_N / H;
        const int rowbase = cy * GRID_N;
        for (int x = 0; x < W; x++) {
            int idx  = y * W + x;
            int cell = rowbase + (x * GRID_N / W);
            cell_total[cell]++;
            if (abs((int) s_cur[idx] - (int) s_bg[idx]) > PIX_DIFF_THR)
                cell_changed[cell]++;
        }
    }

    /* sensitivity 0 → 11 % must change, 50 → 6 %, 100 → 1 %; used both as the
     * overall in-zone threshold and as the per-cell "this cell moved" test. */
    int area_thr = 1 + (100 - g_settings.motion_sensitivity) / 10;
    uint64_t zone = g_settings.detect_zone;
    long zone_px = 0, zone_changed = 0;
    int minc = GRID_N, minr = GRID_N, maxc = -1, maxr = -1;
    for (int c = 0; c < GRID_N * GRID_N; c++) {
        if (!(zone & (1ULL << c))) continue;         /* cell masked out of zone */
        zone_px      += cell_total[c];
        zone_changed += cell_changed[c];
        if (cell_total[c] > 0 &&
            cell_changed[c] * 100 / cell_total[c] >= area_thr) {
            int cc = c % GRID_N, cr = c / GRID_N;
            if (cc < minc) minc = cc;
            if (cc > maxc) maxc = cc;
            if (cr < minr) minr = cr;
            if (cr > maxr) maxr = cr;
        }
    }
    if (zone_px == 0) return false;    /* empty zone → detection off everywhere */

    int  pct    = (int) (zone_changed * 100 / zone_px);
    bool motion = pct >= area_thr;

    if (!motion) {
        /* roll the background on quiet frames — whole frame, so masked-out
         * cells (a swaying branch) are still absorbed and never linger. */
        for (int i = 0; i < px; i++)
            s_bg[i] = (uint8_t) (((int) s_bg[i] * 7 + s_cur[i]) / 8);
        return false;
    }

    /* Build the ROI from the changed cells' bounding box, padded one cell each
     * way for context. No cell crossed the per-cell bar (diffuse change, e.g.
     * light) → empty ROI, i.e. classify the whole frame. */
    if (maxc >= minc && maxr >= minr) {
        if (--minc < 0) minc = 0;
        if (--minr < 0) minr = 0;
        if (++maxc > GRID_N - 1) maxc = GRID_N - 1;
        if (++maxr > GRID_N - 1) maxr = GRID_N - 1;
        s_roi.x0 = (float) minc / GRID_N;
        s_roi.y0 = (float) minr / GRID_N;
        s_roi.x1 = (float) (maxc + 1) / GRID_N;
        s_roi.y1 = (float) (maxr + 1) / GRID_N;
    } else {
        s_roi = roi_none();
    }
    ESP_LOGI(TAG, "motion: %d%% of zone changed (threshold %d%%), roi [%.2f,%.2f]-[%.2f,%.2f]",
             pct, area_thr, s_roi.x0, s_roi.y0, s_roi.x1, s_roi.y1);
    return true;
}

static void capture_event(roi_t roi)
{
    char first_path[96] = "";
    int  frames = 0;

    for (int i = 0; i < g_settings.capture_count; i++) {
        camera_fb_t *fb = camera_grab();
        if (fb) {
            esp_err_t err = capture_event_frame(fb->buf, fb->len,
                                                frames == 0 ? first_path : NULL,
                                                sizeof(first_path));
            camera_return(fb);
            if (err == ESP_OK) frames++;
            else if (frames == 0) break;   /* no SD — don't spin on a dead write path */
        }
        if (i + 1 >= g_settings.capture_count) break;
        vTaskDelay(pdMS_TO_TICKS(g_settings.capture_interval_ms));
        if (!detect_once()) break;         /* visitor left early */
    }

    /* Classify the trigger-time ROI, not whatever the last follow-up frame
     * left in s_roi — the caller passed a snapshot taken at trigger. */
    capture_event_finish(frames, first_path, roi);
}

static void motion_task(void *arg)
{
    for (;;) {
        if (!s_detect_enabled) {
            /* Maintenance pause: drop the baseline so detection re-seeds
             * against the current scene when it resumes (light/subject may
             * have changed while paused), and don't touch the camera. */
            s_have_bg = false;
            vTaskDelay(pdMS_TO_TICKS(DETECT_PERIOD_MS));
            continue;
        }
        if (detect_once()) {
            s_motion_active = true;
            s_trigger_count++;
            capture_event(s_roi);   /* snapshot the trigger ROI for species ID */
            s_motion_active = false;
            vTaskDelay(pdMS_TO_TICKS((uint32_t) g_settings.cooldown_s * 1000));
            s_have_bg = false;   /* re-baseline: light/scene may have shifted */
        } else {
            vTaskDelay(pdMS_TO_TICKS(DETECT_PERIOD_MS));
        }
    }
}

esp_err_t motion_start(void)
{
    if (!camera_available()) {
        ESP_LOGW(TAG, "no camera — motion detection disabled");
        return ESP_OK;
    }
    s_bg  = heap_caps_malloc(DETECT_MAX_PX,     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cur = heap_caps_malloc(DETECT_MAX_PX,     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rgb = heap_caps_malloc(DETECT_MAX_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg || !s_cur || !s_rgb) {
        ESP_LOGE(TAG, "no memory for detection buffers");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(motion_task, "motion", 8192, NULL, 4, NULL) != pdPASS)
        return ESP_FAIL;
    ESP_LOGI(TAG, "motion detection running (sensitivity %u, cooldown %u s)",
             g_settings.motion_sensitivity, g_settings.cooldown_s);
    return ESP_OK;
}

bool     motion_active(void)        { return s_motion_active; }
uint32_t motion_trigger_count(void) { return s_trigger_count; }

bool motion_detection_enabled(void)            { return s_detect_enabled; }
void motion_set_detection_enabled(bool enabled) { s_detect_enabled = enabled; }
