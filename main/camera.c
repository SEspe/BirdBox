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
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "camera";

static bool s_available = false;

/* ── Watchdog state (FSD §3.5) ──────────────────────────────────────────────
 * The v0.19.0 first cut held a mutex across esp_camera_fb_get(); a live test
 * exposed why that's fatal: when the camera streams corrupt frames, fb_get()
 * spins CPU-bound instead of returning, so the grab holds the mutex forever
 * and recovery can never run — and, worse, the spinning grab (in the prio-4
 * motion task) starved app_main so the watchdog never even started.
 *
 * This design never blocks a grab behind a lock. A tiny critical section only
 * guards a few counters:
 *   s_recovering — set while re-initing; new grabs bail (return NULL) so none
 *                  enter fb_get() during esp_camera_deinit().
 *   s_inflight   — grabs currently inside fb_get() OR holding a not-yet-
 *                  returned frame; recovery waits for this to hit 0 before
 *                  deinit (no caller left with a freed framebuffer). If it
 *                  can't drain, a grab is wedged in the driver — unrecoverable
 *                  in firmware, so we flag a fault instead of crashing.
 *   s_attempts   — total grab attempts; lets the watchdog tell "camera stalled
 *                  while consumers are actively grabbing" from "nobody's
 *                  grabbing" (idle), with no probe grab of its own to wedge on.
 * The watchdog runs ABOVE the motion task's priority and starts before it, so
 * a spinning grab can't starve it (see camera_watchdog_start / main.c). */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     s_recovering  = false;
static volatile int      s_inflight    = 0;
static volatile uint32_t s_attempts    = 0;
static volatile int64_t  s_last_good_us= 0;     /* esp_timer of last good frame*/
static volatile uint32_t s_recoveries  = 0;     /* successful re-inits         */
static volatile int64_t  s_last_recov_us = 0;   /* esp_timer of last recovery  */
static volatile bool     s_fault       = false;
/* Frame size actually running, as a RES index — CAMERA_RES_NONE until a size
 * initializes. Kept separate from g_settings.resolution (the user's *request*)
 * because the degrade ladder below can boot at something smaller: writing the
 * fallback back into the setting silently rewrote the user's choice, and any
 * later /api/settings save then persisted it to NVS for good (v1.96). */
static volatile uint8_t  s_active_idx  = CAMERA_RES_NONE;

static esp_err_t camera_hw_init(void);          /* shared boot + recovery init */

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
    return camera_hw_init();   /* s_mux is statically initialized */
}

/* The actual esp_camera_init sequence, shared by boot and by watchdog
 * recovery. Callers other than camera_init() (i.e. recovery) already hold
 * s_cam_mtx. */
static esp_err_t camera_hw_init(void)
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
     * (classify_init runs first, in main.c, so the model is already reserved),
     * and a sensor still wedged from before a *soft* reboot can fail init at
     * any size. Step down through the RES table until one works, rather than
     * failing boot (which would trigger OTA rollback). What actually came up
     * lands in s_active_idx — never in g_settings.resolution, which stays the
     * user's standing request so the next boot retries it (v1.96). */
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
        s_active_idx = CAMERA_RES_NONE;
        return err;
    }
    s_active_idx = idx;
    if (idx != res_idx())
        ESP_LOGW(TAG, "running at %s, not the requested %s — reboot to retry "
                 "(the saved setting is unchanged)",
                 RES[idx].str, RES[res_idx()].str);

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "camera ready — sensor PID 0x%04x, %s", s ? s->id.PID : 0,
             RES[idx].str);
    s_available = true;
    camera_set_rotation(g_settings.rotation);   /* settings_load ran first */
    camera_set_contrast(g_settings.contrast);
    camera_set_ae_level(g_settings.ae_level);
    camera_set_fast_shutter(false);   /* always boot/recover into normal auto
                                         exposure — motion.c's ambient-dark
                                         check (FSD v1.38) decides from here
                                         whether fast_shutter should engage */
    s_last_good_us = esp_timer_get_time();       /* seed heartbeat: no false
                                                    stall before the 1st grab */
    return ESP_OK;
}

bool camera_available(void) { return s_available; }

/* ── Frame access + watchdog (FSD §3.5) ───────────────────────────────────── */

camera_fb_t *camera_grab(void)
{
    /* Reserve a slot atomically: bail if a recovery is in progress (so we
     * never enter fb_get() while it deinits) or the camera is down. */
    portENTER_CRITICAL(&s_mux);
    if (s_recovering || !s_available) { portEXIT_CRITICAL(&s_mux); return NULL; }
    s_inflight++;
    s_attempts++;
    portEXIT_CRITICAL(&s_mux);

    camera_fb_t *fb = esp_camera_fb_get();          /* NO lock held here       */

    if (fb) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_mux);
        s_last_good_us = now;      /* s_inflight stays up until camera_return  */
        portEXIT_CRITICAL(&s_mux);
        return fb;
    }
    portENTER_CRITICAL(&s_mux);                      /* NULL: nothing held      */
    if (s_inflight > 0) s_inflight--;
    portEXIT_CRITICAL(&s_mux);
    return NULL;
}

void camera_return(camera_fb_t *fb)
{
    if (!fb) return;
    esp_camera_fb_return(fb);
    portENTER_CRITICAL(&s_mux);
    if (s_inflight > 0) s_inflight--;
    portEXIT_CRITICAL(&s_mux);
}

/* Soft recovery: block new grabs, wait for in-flight ones to drain, deinit,
 * hold XCLK low so the sensor PLL settles, then re-init. Clears ESP32-side
 * LCD_CAM/DMA latches, XCLK glitches, and the fb_get()-returns-NULL latch.
 *
 * Returns ESP_ERR_INVALID_STATE if a grab is wedged inside the driver and
 * won't drain — deinit()ing then would crash the wedged task, so we can't
 * safely re-init in firmware. That (and the OV2640 analog-core latch, which
 * survives even a full reboot) needs the sensor's VDD removed, which this
 * board can't do: PWDN/RESET are unwired. The watchdog turns that into a
 * loud camFault instead. See FSD §3.5 for the HW mod (wire PWDN / a MOSFET on
 * the camera rail) that would let this power-cycle for real. */
static esp_err_t camera_recover(void)
{
    portENTER_CRITICAL(&s_mux);
    s_recovering = true;                            /* new grabs now bail       */
    portEXIT_CRITICAL(&s_mux);

    int64_t t0 = esp_timer_get_time();              /* drain in-flight grabs    */
    int n;
    do {
        portENTER_CRITICAL(&s_mux); n = s_inflight; portEXIT_CRITICAL(&s_mux);
        if (n == 0) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() - t0 < 2000000);

    if (n != 0) {                                   /* a grab is stuck in fb_get*/
        portENTER_CRITICAL(&s_mux); s_recovering = false; portEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "watchdog: grab wedged in driver (inflight=%d) — cannot "
                 "re-init safely", n);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "watchdog: re-initializing camera");
    s_available = false;
    esp_camera_deinit();

    gpio_reset_pin(CAM_PIN_XCLK);                   /* hold clock low to drain  */
    gpio_set_direction(CAM_PIN_XCLK, GPIO_MODE_OUTPUT);
    gpio_set_level(CAM_PIN_XCLK, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_err_t err = camera_hw_init();               /* re-inits LEDC/XCLK too   */
    s_recoveries++;
    s_last_recov_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    s_recovering = false;                           /* grabs resume (if init ok)*/
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGW(TAG, "watchdog: re-init %s (recovery #%lu)",
             err == ESP_OK ? "ok" : esp_err_to_name(err),
             (unsigned long) s_recoveries);
    return err;
}

static void cam_wd_task(void *arg)
{
    const int64_t STALL_US = 5000000;    /* no good frame this long ⇒ suspect  */
    uint32_t recover_fail = 0;
    uint32_t last_att = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_available || s_recovering) continue;

        int64_t  now;
        uint32_t att;
        int      inflight;
        int64_t  last_good;
        portENTER_CRITICAL(&s_mux);
        att = s_attempts; inflight = s_inflight; last_good = s_last_good_us;
        portEXIT_CRITICAL(&s_mux);
        now = esp_timer_get_time();

        /* A real stall = no good frame for STALL_US WHILE consumers are trying
         * (attempts advancing, or a grab still in flight). If nobody's grabbing
         * the heartbeat is stale but the camera is merely idle — leave it. No
         * probe grab, so the watchdog can't wedge itself on a broken camera. */
        bool active = (att != last_att) || (inflight > 0);
        last_att = att;
        if (!active || now - last_good < STALL_US) continue;

        ESP_LOGW(TAG, "watchdog: camera stalled %llds (inflight=%d) — recovering #%lu",
                 (long long)((now - last_good) / 1000000), inflight,
                 (unsigned long)(s_recoveries + 1));

        if (camera_recover() == ESP_ERR_INVALID_STATE) {
            s_fault = true;                          /* wedged in driver         */
            ESP_LOGE(TAG, "watchdog: camera unrecoverable in firmware — a manual "
                     "power cycle is required (board has no PWDN line, FSD §3.5)");
            vTaskDelay(pdMS_TO_TICKS(30000));        /* back off, don't spin/spam*/
            portENTER_CRITICAL(&s_mux); last_att = s_attempts; portEXIT_CRITICAL(&s_mux);
            continue;
        }

        /* Judge success by whether a real frame lands after re-init (the seed
         * in camera_hw_init predates judge_from, so it can't false-pass). */
        int64_t judge_from = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(1500));
        portENTER_CRITICAL(&s_mux); last_good = s_last_good_us; portEXIT_CRITICAL(&s_mux);
        if (last_good > judge_from) {
            recover_fail = 0;
            s_fault = false;
            ESP_LOGI(TAG, "watchdog: camera recovered (recovery #%lu)",
                     (unsigned long) s_recoveries);
        } else if (++recover_fail >= 3) {
            s_fault = true;
            ESP_LOGE(TAG, "watchdog: %lu recoveries failed — camera FAULT, a "
                     "manual power cycle is required (FSD §3.5)",
                     (unsigned long) recover_fail);
            vTaskDelay(pdMS_TO_TICKS(30000));        /* back off after giving up */
        }
        portENTER_CRITICAL(&s_mux); last_att = s_attempts; portEXIT_CRITICAL(&s_mux);
    }
}

esp_err_t camera_watchdog_start(void)
{
    if (!s_available) {
        ESP_LOGW(TAG, "no camera — watchdog not started");
        return ESP_OK;
    }
    /* Priority 6 — ABOVE the motion task (prio 4). If a broken camera makes a
     * grab spin CPU-bound in the motion task, the watchdog must still be
     * schedulable to detect it and recover; and it's started before motion
     * (main.c) so that spin can't keep it from ever launching. */
    if (xTaskCreate(cam_wd_task, "cam_wd", 4096, NULL, 6, NULL) != pdPASS)
        return ESP_FAIL;
    ESP_LOGI(TAG, "camera watchdog running");
    return ESP_OK;
}

uint32_t camera_recovery_count(void) { return s_recoveries; }
bool     camera_fault(void)          { return s_fault; }

int camera_last_recovery_ago_s(void)
{
    if (!s_last_recov_us) return -1;
    return (int) ((esp_timer_get_time() - s_last_recov_us) / 1000000);
}

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

esp_err_t camera_set_ae_level(int level)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    if (level < -2) level = -2;
    if (level >  2) level =  2;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_ae_level(s, level) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "ae_level set to %d", level);
    return ESP_OK;
}

/* Out of the OV2640's 0-1200 AEC range; picked as a "usually well under what
 * auto-exposure picks in daylight" starting point (its exact meaning in
 * exposure-time depends on clock config, not documented in µs) — the
 * noise/darkness trade-off is inherent to fixing exposure short, and this
 * may want field tuning against real captures once blur is visibly worse or
 * better at this value. */
#define FAST_SHUTTER_AEC_VALUE  300

esp_err_t camera_set_fast_shutter(bool enable)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_FAIL;
    if (enable) {
        /* AGC stays auto so the image doesn't just go dark; only exposure
         * time is pinned short, which is the actual blur-causing knob. */
        if (s->set_gain_ctrl(s, 1) != 0 ||
            s->set_exposure_ctrl(s, 0) != 0 ||
            s->set_aec_value(s, FAST_SHUTTER_AEC_VALUE) != 0)
            return ESP_FAIL;
    } else {
        if (s->set_exposure_ctrl(s, 1) != 0)   /* back to normal auto exposure */
            return ESP_FAIL;
    }
    ESP_LOGI(TAG, "fast shutter %s", enable ? "on" : "off");
    return ESP_OK;
}

/* The size actually running, not the requested one — those differ after a
 * degrade, and after a saved-but-not-yet-rebooted change (v1.96). */
const char *camera_framesize_str(void)
{
    uint8_t a = s_active_idx;
    return a < RES_COUNT ? RES[a].str : "none (camera down)";
}

uint8_t camera_active_res(void) { return s_active_idx; }

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
