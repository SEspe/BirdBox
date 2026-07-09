/* Motion detection + visit-event orchestration (FSD §3.1).
 *
 * The sensor delivers JPEG (needed for stream/captures), so detection frames
 * are made by decoding each JPEG at 1/8 scale (SVGA → 100×75) with the
 * esp_jpeg component and reducing RGB565 to grayscale. ~7.5 kpx per compare
 * at ~4 Hz is cheap, and needs no sensor reconfiguration.
 *
 * Rolling background: two exponential moving averages in parallel, both
 * updated only while no motion is seen (so a bird sitting still doesn't get
 * absorbed into either one mid-visit) and re-seeded together after each
 * event's cool-down (light may have changed during the visit). The fast one
 * (7/8 old + 1/8 new, ~1 s half-life) tracks normal lighting drift; the slow
 * one (63/64 old + 1/64 new, ~11 s half-life) deliberately lags, so a small
 * or low-contrast bird that hops in gradually — and would otherwise get
 * partly absorbed by the fast EMA before ever crossing the trigger threshold
 * — still stands out against the slow one for longer. A pixel counts as
 * changed if it diverges from either background.
 *
 * Event pipeline (one task, which also serializes all its SD writes):
 * trigger → save first frame immediately → up to capture_count-1 follow-ups
 * at capture_interval_ms while motion persists → visit-log row → cool-down. */
#include "motion.h"
#include "camera.h"
#include "capture.h"
#include "settings.h"
#include "illum.h"

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

/* Ambient-dark detection (FSD v1.36/v1.38): reuses the grayscale detect
 * frame already decoded every DETECT_PERIOD_MS, no extra sampling, to drive
 * two independent auto behaviours — the illuminator (ir_led_mode==1) and
 * fast-shutter blur reduction (fast_shutter==1). Wide hysteresis gap between
 * the two thresholds (0-255 avg luma) means ordinary daylight can't
 * false-trigger a flicker loop.
 *
 * Both consumers bias this same reading, in opposite directions: the
 * illuminator brightens its own frame (self-raising — harmless, daylight
 * still swamps it, see below), while fast-shutter's pinned-short exposure
 * reads *darker* than full auto would for the same light (self-lowering).
 * A live bug (FSD v1.37) showed why fast-shutter can't just be a static
 * on/off setting: forced short exposure only looks right at the light level
 * it was tuned against — as ambient light rose through the day with AEC
 * disabled, the image overexposed and couldn't correct itself. Gating it on
 * this same dark/bright signal means it only ever engages when the scene is
 * genuinely too dim for the fixed short exposure to overexpose. */
#define AMBIENT_DARK_ON_THR   35   /* avg luma below this -> too dark, turn on */
#define AMBIENT_DARK_OFF_THR  90   /* avg luma above this -> bright enough, turn off */

static uint8_t *s_bg, *s_bg_slow, *s_cur, *s_rgb;
static bool     s_have_bg = false;
static bool     s_dark     = false;   /* hysteresis-debounced ambient state */
static bool     s_illum_on = false;
static bool     s_fshut_on = false;
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
    long luma_sum = 0;
    for (int i = 0; i < px; i++) {
        s_cur[i] = (uint8_t) (((rgb[i] >> 5) & 0x3F) << 2);
        luma_sum += s_cur[i];
    }

    /* Runs on every decoded frame regardless of motion/background state, so
     * both auto behaviours track ambient light continuously rather than only
     * during a visit — "on during dark hours", not a per-shot flash/trigger. */
    int avg = (int) (luma_sum / px);
    if (!s_dark && avg < AMBIENT_DARK_ON_THR)       s_dark = true;
    else if (s_dark && avg > AMBIENT_DARK_OFF_THR)  s_dark = false;

    bool want_illum = (g_settings.ir_led_mode == 1) && s_dark;
    if (want_illum != s_illum_on) { illum_set(want_illum); s_illum_on = want_illum; }

    bool want_fshut = (g_settings.fast_shutter == 1) && s_dark;
    if (want_fshut != s_fshut_on) { camera_set_fast_shutter(want_fshut); s_fshut_on = want_fshut; }

    if (!s_have_bg) {
        memcpy(s_bg, s_cur, px);
        memcpy(s_bg_slow, s_cur, px);
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
            /* Changed if it diverges from EITHER background — the fast one
             * (adapts in ~1 s) catches a normal arrival, the slow one (~11 s
             * half-life) still shows a bird that hopped in gradually and got
             * partly absorbed by the fast EMA before ever crossing threshold. */
            if (abs((int) s_cur[idx] - (int) s_bg[idx])      > PIX_DIFF_THR ||
                abs((int) s_cur[idx] - (int) s_bg_slow[idx]) > PIX_DIFF_THR)
                cell_changed[cell]++;
        }
    }

    /* sensitivity 0 → 11 % must change, 50 → 6 %, 100 → 1 %; used both as the
     * overall in-zone threshold and as the per-cell "this cell moved" test. */
    int area_thr = 1 + (100 - g_settings.motion_sensitivity) / 10;
    uint64_t zone = g_settings.detect_zone;
    long zone_px = 0, zone_changed = 0;
    bool moved[GRID_N * GRID_N];
    for (int c = 0; c < GRID_N * GRID_N; c++) {
        moved[c] = false;
        if (!(zone & (1ULL << c))) continue;         /* cell masked out of zone */
        zone_px      += cell_total[c];
        zone_changed += cell_changed[c];
        if (cell_total[c] > 0 &&
            cell_changed[c] * 100 / cell_total[c] >= area_thr)
            moved[c] = true;
    }
    if (zone_px == 0) return false;    /* empty zone → detection off everywhere */

    int  pct    = (int) (zone_changed * 100 / zone_px);
    bool motion = pct >= area_thr;

    if (!motion) {
        /* roll both backgrounds on quiet frames — whole frame, so masked-out
         * cells (a swaying branch) are still absorbed and never linger. Fast
         * EMA (7/8) tracks normal lighting drift; slow EMA (63/64, ~11 s
         * half-life) deliberately lags behind so a gradual arrival still
         * shows up against it even after the fast one has absorbed it. */
        for (int i = 0; i < px; i++) {
            s_bg[i]      = (uint8_t) (((int) s_bg[i]      * 7  + s_cur[i]) / 8);
            s_bg_slow[i] = (uint8_t) (((int) s_bg_slow[i] * 63 + s_cur[i]) / 64);
        }
        return false;
    }

    /* ROI = bounding box of the LARGEST 4-connected cluster of moved cells
     * (weighted by changed pixels), padded one cell each way for context. Two
     * separate movers (bird + in-zone shadow/feeder swing) no longer merge
     * into one diluted bbox — the zoom tracks the dominant object. No cell
     * crossed the per-cell bar (diffuse change, e.g. light) → empty ROI,
     * i.e. classify the whole frame. */
    bool seen[GRID_N * GRID_N] = {false};
    long best_wt = -1;
    int  minc = 0, minr = 0, maxc = -1, maxr = -1;
    for (int c0 = 0; c0 < GRID_N * GRID_N; c0++) {
        if (!moved[c0] || seen[c0]) continue;
        int  stack[GRID_N * GRID_N], sp = 0;
        long wt = 0;
        int  lminc = GRID_N, lminr = GRID_N, lmaxc = -1, lmaxr = -1;
        stack[sp++] = c0;
        seen[c0] = true;
        while (sp > 0) {
            int c  = stack[--sp];
            int cc = c % GRID_N, cr = c / GRID_N;
            wt += cell_changed[c];
            if (cc < lminc) lminc = cc;
            if (cc > lmaxc) lmaxc = cc;
            if (cr < lminr) lminr = cr;
            if (cr > lmaxr) lmaxr = cr;
            const int nb[4] = { cc > 0          ? c - 1      : -1,
                                cc < GRID_N - 1 ? c + 1      : -1,
                                cr > 0          ? c - GRID_N : -1,
                                cr < GRID_N - 1 ? c + GRID_N : -1 };
            for (int k = 0; k < 4; k++)
                if (nb[k] >= 0 && moved[nb[k]] && !seen[nb[k]]) {
                    seen[nb[k]] = true;
                    stack[sp++] = nb[k];
                }
        }
        if (wt > best_wt) {
            best_wt = wt;
            minc = lminc; minr = lminr; maxc = lmaxc; maxr = lmaxr;
        }
    }
    if (maxc >= minc && maxr >= minr && best_wt >= 0) {
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
    char  first_path[96] = "";
    int   frames = 0;
    roi_t cur = roi;   /* trigger-time ROI for frame 0 */

    for (int i = 0; i < g_settings.capture_count; i++) {
        camera_fb_t *fb = camera_grab();
        if (fb) {
            esp_err_t err = capture_event_frame(fb->buf, fb->len, cur,
                                                frames == 0 ? first_path : NULL,
                                                sizeof(first_path));
            camera_return(fb);
            if (err == ESP_OK) frames++;
            else if (frames == 0) break;   /* no SD — don't spin on a dead write path */
        }
        if (i + 1 >= g_settings.capture_count) break;
        vTaskDelay(pdMS_TO_TICKS(g_settings.capture_interval_ms));
        if (!detect_once()) break;         /* visitor left early */
        cur = s_roi;   /* fresh ROI: the zoom follows the bird between frames */
    }

    capture_event_finish(frames, first_path);
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
    s_bg      = heap_caps_malloc(DETECT_MAX_PX,     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bg_slow = heap_caps_malloc(DETECT_MAX_PX,     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cur     = heap_caps_malloc(DETECT_MAX_PX,     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rgb     = heap_caps_malloc(DETECT_MAX_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg || !s_bg_slow || !s_cur || !s_rgb) {
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
