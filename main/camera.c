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
#if CONFIG_CAMERA_AF_SUPPORT
#include "esp_camera_af.h"
#endif
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
/* Incremented on every SUCCESSFUL hw init (boot, watchdog recovery, night
 * wake). Lets readers outside this file detect that their cached sensor state
 * is stale — see camera_init_generation(). */
static volatile uint32_t s_init_gen    = 0;
static volatile uint32_t s_fault_clears= 0;     /* times a real frame cleared it (v2.97) */
/* Frame size actually running, as a RES index — CAMERA_RES_NONE until a size
 * initializes. Kept separate from g_settings.resolution (the user's *request*)
 * because the degrade ladder below can boot at something smaller: writing the
 * fallback back into the setting silently rewrote the user's choice, and any
 * later /api/settings save then persisted it to NVS for good (v1.96). */
static volatile uint8_t  s_active_idx  = CAMERA_RES_NONE;

static esp_err_t camera_hw_init(void);          /* shared boot + recovery init */

/* Selectable frame sizes (Settings → resolution, FSD §5). The index is what's
 * stored in g_settings.resolution; higher = more detail on small/distant birds
 * at the cost of PSRAM + a slower detect loop. Applied at camera_init, so a
 * change needs a reboot (buffers are sized here).
 *
 * Indices 0-4 are frozen — they are already in NVS on every deployed box, so
 * new sizes may only be APPENDED. Entries must stay in ascending framesize_t
 * order: caps_probe() walks the table against the driver's max_size and stops
 * at the first size the sensor can't do.
 *
 * The old ceiling was SXGA, on the grounds that UXGA left no PSRAM for the
 * classifier's JPEG-decode buffer. That reasoning died with the on-device model
 * (0.74.0): the TFLM arena is gone and PSRAM idles at ~7.9 MB free, while
 * classify.cpp's decode halves itself until it fits CLS_DECODE_MAX, so a 5 MP
 * frame simply decodes at a coarser scale instead of failing. What the ceiling
 * is now is the SENSOR — an OV2640 tops out at UXGA, an OV5640 at QSXGA.
 *
 * Bigger is NOT automatically better here: the 5:4 and 4:3 sizes give a
 * narrower field of view than 16:9 HD, and every extra pixel slows the 250 ms
 * detect loop, which is why HD stays the default and the UI labels say so
 * (v1.21/v2.15 measured HD > SXGA for motion despite SXGA's extra pixels). The
 * big sizes are there for a well-lit feeder at distance, not as an upgrade. */
static const struct { framesize_t fs; const char *str; } RES[] = {
    { FRAMESIZE_VGA,   "VGA 640x480"     },   /* 0 */
    { FRAMESIZE_SVGA,  "SVGA 800x600"    },   /* 1 */
    { FRAMESIZE_XGA,   "XGA 1024x768"    },   /* 2 */
    { FRAMESIZE_HD,    "HD 1280x720"     },   /* 3 — default */
    { FRAMESIZE_SXGA,  "SXGA 1280x1024"  },   /* 4 — OV2640 ceiling until v2.81 */
    { FRAMESIZE_UXGA,  "UXGA 1600x1200"  },   /* 5 — OV2640 ceiling */
    { FRAMESIZE_QXGA,  "QXGA 2048x1536"  },   /* 6 — OV3660 ceiling */
    { FRAMESIZE_QSXGA, "QSXGA 2560x1920" },   /* 7 — OV5640 ceiling */
};
#define RES_COUNT (sizeof(RES) / sizeof(RES[0]))

/* Filled by caps_probe() after esp_camera_init succeeds. Static init is the
 * no-camera answer, so camera_caps() is safe to call before/without a sensor. */
static camera_caps_t s_caps = {
    .pid = 0, .name = "none", .max_res = CAMERA_RES_NONE,
    .sharpness = false, .denoise = false, .autofocus = false,
};

/* Why autofocus isn't available, kept for the Debug card. "AF is off" and "AF
 * was asked for and the sensor wouldn't take the firmware" are very different
 * answers for someone deciding whether their module has a VCM lens, and
 * without this the UI can only say "no autofocus" to both. */
static esp_err_t s_af_err = ESP_ERR_NOT_SUPPORTED;

/* The user's requested size, clamped to what the firmware AND the sensor can
 * do. Before the probe runs (i.e. the first camera_init) max_res is
 * CAMERA_RES_NONE, so only the table bound applies and the degrade ladder in
 * camera_hw_init sorts out a sensor that can't manage the request. */
static uint8_t res_idx(void)
{
    uint8_t idx = g_settings.resolution < RES_COUNT ? g_settings.resolution
                                                    : 1;   /* stale/bogus -> SVGA */
    if (s_caps.max_res != CAMERA_RES_NONE && idx > s_caps.max_res)
        idx = s_caps.max_res;
    return idx;
}

/* Identify the sensor and work out what it supports. Called once per successful
 * init (boot and every watchdog recovery — a recovery can in principle come up
 * on a different answer, and re-probing is cheap).
 *
 * Capabilities are probed, not looked up by PID, so an OV3660 or a sensor added
 * to the driver later behaves correctly without a change here:
 *   max_res    — the driver's own camera_sensor[] max_size, walked against RES
 *   sharpness  — set_sharpness(0) returns -1 on the OV2640's stub, 0 on a
 *                sensor that has one. Level 0 is the neutral value we'd apply
 *                anyway, so the probe has no side effect worth undoing.
 *   denoise    — same trick with set_denoise(0) (0 = off, also neutral).
 *   autofocus  — AF firmware download; see the caveat in camera.h. */
static void caps_probe(sensor_t *s)
{
    if (!s) { s_caps.pid = 0; s_caps.name = "none";
              s_caps.max_res = CAMERA_RES_NONE;
              s_caps.sharpness = s_caps.denoise = s_caps.autofocus = false;
              return; }

    s_caps.pid = s->id.PID;

    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    s_caps.name = info && info->name ? info->name : "unknown";

    /* Highest RES entry at or below the sensor's max. RES is ascending, so the
     * last one that fits wins; a sensor smaller than RES[0] (VGA) leaves this
     * at 0 rather than CAMERA_RES_NONE — VGA is then the only option and the
     * degrade ladder still has somewhere to land. */
    uint8_t maxr = 0;
    if (info) {
        for (uint8_t i = 0; i < RES_COUNT; i++)
            if (RES[i].fs <= info->max_size) maxr = i;
    } else {
        maxr = 4;                      /* unknown sensor: the pre-v2.81 ceiling */
    }
    s_caps.max_res = maxr;

    s_caps.sharpness = (s->set_sharpness && s->set_sharpness(s, 0) == 0);
    s_caps.denoise   = (s->set_denoise   && s->set_denoise(s, 0)   == 0);

#if CONFIG_CAMERA_AF_SUPPORT
    /* Loading the AF firmware pushes ~4 KB into the sensor's MCU one SCCB
     * register at a time, then waits for its ucode to report idle — and only
     * makes sense if the operator asked for autofocus, so an OV2640 board never
     * pays for it. The timeout covers the idle wait only (the upload is
     * unbounded); 10 s is deliberately far more than a healthy module needs
     * (~100-300 ms), so a timeout here means the AF ucode never came up rather
     * than that we were impatient. That distinction is the whole diagnostic:
     * plenty of OV5640 modules are fixed-focus with no VCM motor fitted, and
     * they look exactly like this. */
    s_caps.autofocus = false;
    s_af_err = ESP_ERR_NOT_SUPPORTED;
    if (g_settings.focus_mode != FOCUS_OFF && esp_camera_af_is_supported(s)) {
        esp_camera_af_config_t afc = { .mode = ESP_CAMERA_AF_MODE_AUTO,
                                       .timeout_ms = 10000 };
        s_af_err = esp_camera_af_init(s, &afc);
        s_caps.autofocus = (s_af_err == ESP_OK);
        if (s_af_err != ESP_OK)
            ESP_LOGW(TAG, "autofocus requested but AF init failed (%s) — the "
                     "module is probably fixed-focus (no VCM lens)",
                     esp_err_to_name(s_af_err));
    }
#else
    s_caps.autofocus = false;
    s_af_err = ESP_ERR_NOT_SUPPORTED;
#endif

    ESP_LOGI(TAG, "sensor %s (PID 0x%04x): max %s%s%s%s", s_caps.name,
             (unsigned) s_caps.pid, RES[s_caps.max_res].str,
             s_caps.sharpness ? ", sharpness" : "",
             s_caps.denoise   ? ", denoise"   : "",
             s_caps.autofocus ? ", autofocus" : "");
}

const camera_caps_t *camera_caps(void) { return &s_caps; }
uint8_t camera_res_count(void)         { return RES_COUNT; }
const char *camera_res_str(uint8_t idx)
{
    return idx < RES_COUNT ? RES[idx].str : NULL;
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
    caps_probe(s);              /* before the setters below — they consult it */
    s_available = true;
    camera_set_view(g_settings.rot_deg,         /* settings_load ran first */
                    g_settings.mirror_h, g_settings.mirror_v);
    camera_set_contrast(g_settings.contrast);
    camera_set_ae_level(g_settings.ae_level);
    camera_set_sharpness(g_settings.sharpness); /* no-ops on a sensor without */
    camera_set_denoise(g_settings.denoise);     /* these — see camera_caps    */
    camera_set_focus(g_settings.focus_mode, g_settings.focus_pos);
    camera_set_fast_shutter(false);   /* always boot/recover into normal auto
                                         exposure — motion.c's ambient-dark
                                         check (FSD v1.38) decides from here
                                         whether fast_shutter should engage */
    s_last_good_us = esp_timer_get_time();       /* seed heartbeat: no false
                                                    stall before the 1st grab */
    /* Bump LAST, and only on success. Anything outside this file that caches
     * sensor register state must notice that the sensor was just reset and
     * re-apply it — see camera_init_generation() in camera.h. The line above is
     * exactly why: fast shutter is forced off here, and motion.c's cached
     * "it is already on" flag used to survive that, so after any wake or
     * watchdog recovery the box ran with full auto exposure while believing it
     * had a short one. That silently inverted night detection (FSD §14). */
    s_init_gen++;
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
        /* A DELIVERED FRAME DISPROVES THE FAULT (v2.97). s_fault means "auto-
         * recovery gave up, a manual power cycle is required" — but nothing
         * cleared it if the camera came back on its own, because the only reset
         * lived in the watchdog's recovery-SUCCESS branch and a camera that
         * never stalls again never gets a recovery attempt. The flag then stuck
         * for the rest of the uptime, reporting a dead camera on a box that was
         * happily capturing (observed on .111, 2026-08-06: camFault true while
         * POST /api/capture returned a 103 KB frame on the first try).
         * This is the one place a real frame is confirmed, so it is the honest
         * place to clear it. Logged after leaving the critical section —
         * ESP_LOG takes locks and must never run inside one. */
        bool cleared = false;
        portENTER_CRITICAL(&s_mux);
        s_last_good_us = now;      /* s_inflight stays up until camera_return  */
        if (s_fault) { s_fault = false; s_fault_clears++; cleared = true; }
        portEXIT_CRITICAL(&s_mux);
        if (cleared)
            ESP_LOGW(TAG, "camera fault CLEARED — a real frame was delivered "
                          "(self-clear #%lu)", (unsigned long) s_fault_clears);
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

/* ── Night sleep (FSD §14) ──────────────────────────────────────────────────
 * Deliberately powering the sensor down between dusk and dawn, as opposed to
 * camera_recover()'s "the sensor is wedged, re-init it". Same drain-then-
 * deinit dance, because the grab-racing-a-deinit hazard is identical, but the
 * end state differs: recovery re-inits immediately, this one stays down.
 *
 * The watchdog needs no change to tolerate this. It already bails on
 * `!s_available || s_recovering`, and it only acts when consumers are actually
 * grabbing — so a sleeping camera is simply invisible to it rather than
 * something it keeps trying to "fix". */
static volatile bool s_asleep = false;

esp_err_t camera_sleep(void)
{
    if (s_asleep) return ESP_OK;

    portENTER_CRITICAL(&s_mux);
    s_recovering = true;                            /* new grabs bail out      */
    portEXIT_CRITICAL(&s_mux);

    int64_t t0 = esp_timer_get_time();              /* drain in-flight grabs   */
    int n;
    do {
        portENTER_CRITICAL(&s_mux); n = s_inflight; portEXIT_CRITICAL(&s_mux);
        if (n == 0) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() - t0 < 2000000);

    if (n != 0) {                                   /* wedged: leave it alone  */
        portENTER_CRITICAL(&s_mux); s_recovering = false; portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "night: grab wedged (inflight=%d) — staying awake", n);
        return ESP_ERR_INVALID_STATE;
    }

    s_available = false;
    esp_camera_deinit();
    gpio_reset_pin(CAM_PIN_XCLK);                   /* hold the clock low      */
    gpio_set_direction(CAM_PIN_XCLK, GPIO_MODE_OUTPUT);
    gpio_set_level(CAM_PIN_XCLK, 0);
    s_asleep = true;

    portENTER_CRITICAL(&s_mux);
    s_recovering = false;   /* grabs still bail: s_available is false          */
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "night: camera powered down");
    return ESP_OK;
}

esp_err_t camera_wake(void)
{
    if (!s_asleep) return ESP_OK;
    esp_err_t err = camera_hw_init();               /* re-inits LEDC/XCLK too  */
    s_asleep = false;
    /* Re-seed the heartbeat. Without this the watchdog inherits a timestamp
     * from before the whole night and could read a fresh camera as stalled the
     * moment anything starts grabbing again. */
    portENTER_CRITICAL(&s_mux);
    s_last_good_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "night: camera wake %s", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err;
}

bool camera_asleep(void) { return s_asleep; }

/* See camera.h: readers that cache sensor state compare this to spot a reset. */
uint32_t camera_init_generation(void) { return s_init_gen; }

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
        /* Fresh frames mean the camera is healthy again, so the consecutive-
         * failure tally starts over (v2.97). Without this, recover_fail stayed
         * at its terminal 3 for the rest of the uptime once a fault had been
         * declared, and the NEXT stall — however unrelated, hours later — would
         * re-declare a fault on its very first failed recovery instead of
         * getting the intended three tries. That pairs with the self-clear in
         * camera_fb_get: clearing the flag while leaving the tally latched
         * would just make the fault come back at the first hiccup. */
        if (now - last_good < STALL_US) recover_fail = 0;
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
uint32_t camera_fault_clears(void)   { return s_fault_clears; }

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

/* OV5640-class only. The OV2640's set_sharpness/set_denoise are stubs that
 * return -1 for every argument (ov2640.c), which is exactly what caps_probe
 * uses to detect them — so these report ESP_ERR_NOT_SUPPORTED rather than
 * ESP_FAIL, letting the caller tell "this sensor hasn't got one" from "the
 * write failed". Both are no-ops rather than errors at boot on an OV2640. */
esp_err_t camera_set_sharpness(int level)
{
    if (!s_available)      return ESP_ERR_INVALID_STATE;
    if (!s_caps.sharpness) return ESP_ERR_NOT_SUPPORTED;
    if (level < -3) level = -3;
    if (level >  3) level =  3;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_sharpness(s, level) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "sharpness set to %d", level);
    return ESP_OK;
}

esp_err_t camera_set_denoise(int level)
{
    if (!s_available)    return ESP_ERR_INVALID_STATE;
    if (!s_caps.denoise) return ESP_ERR_NOT_SUPPORTED;
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_denoise(s, level) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "denoise set to %d", level);
    return ESP_OK;
}

/* ── Autofocus (FSD §5) ─────────────────────────────────────────────────────
 * Only ever active on a sensor whose AF firmware loaded (see caps_probe). The
 * lock flag is last-known state, refreshed by camera_focus_now(); continuous
 * AUTO mode hunts on its own and is not polled — polling it every frame would
 * put SCCB traffic in the detect loop for a Debug-card cosmetic. */
static volatile bool s_focus_locked = false;

esp_err_t camera_set_focus(uint8_t mode, uint16_t pos)
{
#if CONFIG_CAMERA_AF_SUPPORT
    if (!s_available)      return ESP_ERR_INVALID_STATE;
    if (!s_caps.autofocus) return ESP_ERR_NOT_SUPPORTED;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_FAIL;

    esp_err_t err;
    switch (mode) {
    case FOCUS_AUTO:
        err = esp_camera_af_set_mode(s, ESP_CAMERA_AF_MODE_AUTO);
        break;
    case FOCUS_MANUAL:
        if (pos > 1023) pos = 1023;
        /* Leave the continuous loop first, or it immediately drives the lens
         * back off the position we just set. */
        esp_camera_af_set_mode(s, ESP_CAMERA_AF_MODE_MANUAL);
        err = esp_camera_af_set_manual_position(s, pos);
        break;
    default:                                  /* FOCUS_OFF — park the AF loop */
        err = esp_camera_af_set_mode(s, ESP_CAMERA_AF_MODE_MANUAL);
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "focus mode %u failed: %s", mode, esp_err_to_name(err));
        return err;
    }
    s_focus_locked = false;                   /* unknown until a status read  */
    ESP_LOGI(TAG, "focus mode %u%s", mode,
             mode == FOCUS_MANUAL ? " (manual position applied)" : "");
    return ESP_OK;
#else
    (void) mode; (void) pos;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t camera_focus_now(void)
{
#if CONFIG_CAMERA_AF_SUPPORT
    if (!s_available)      return ESP_ERR_INVALID_STATE;
    if (!s_caps.autofocus) return ESP_ERR_NOT_SUPPORTED;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_FAIL;

    esp_err_t err = esp_camera_af_trigger(s);
    if (err != ESP_OK) return err;

    esp_camera_af_status_t st = {0};
    err = esp_camera_af_wait(s, 2000, &st);
    s_focus_locked = (err == ESP_OK && st.focused);
    ESP_LOGI(TAG, "focus now: %s", s_focus_locked ? "locked" : "no lock");
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool camera_focus_locked(void) { return s_focus_locked; }

const char *camera_af_error(void)
{
    if (s_caps.autofocus) return "";
    if (g_settings.focus_mode == FOCUS_OFF) return "off";
#if !CONFIG_CAMERA_AF_SUPPORT
    return "not built in";
#else
    /* A timeout is the interesting one: the ucode went in but never reported
     * ready, which is what a module with no focus motor does. */
    return s_af_err == ESP_ERR_TIMEOUT       ? "AF firmware timed out (no VCM lens?)"
         : s_af_err == ESP_ERR_NOT_SUPPORTED ? "sensor reports no AF"
         : s_af_err == ESP_OK                ? "init ok but unavailable"
         : esp_err_to_name(s_af_err);
#endif
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

esp_err_t camera_set_view(uint16_t deg, bool mirror_h, bool mirror_v)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_FAIL;
    /* The sensor owns everything it can do for free: both mirrors, and EXACTLY
     * 180 of rotation — which IS both mirrors (a 180 turn is a flip on each
     * axis). So the two compose by XOR: at 180 a requested mirror CANCELS that
     * axis rather than adding to it, which is the correct result and the reason
     * this is one function instead of two.
     *
     * All of it is free, lossless, and lands in the stored JPEG and the
     * classifier input, not just the screen — that is the point of doing it
     * here (v2.89). Mirroring used to be browser-only, so the live view and the
     * file on the card disagreed whenever a mirror was on.
     *
     * 90/270 is the only part left to the browser: no OV sensor rotates a
     * quarter turn in hardware. applyView() must therefore skip its own
     * rotation at 180, or it would be applied twice. */
    bool flip180 = (deg == 180);
    bool hm = flip180 ^ mirror_h;
    bool vf = flip180 ^ mirror_v;
    s->set_hmirror(s, hm);
    s->set_vflip(s, vf);
    ESP_LOGI(TAG, "view %u deg mirror %c%c — sensor applies hmirror=%d vflip=%d, "
                  "browser does %s",
             (unsigned) deg, mirror_h ? 'H' : '-', mirror_v ? 'V' : '-',
             (int) hm, (int) vf,
             (deg == 90 || deg == 270) ? "the quarter turn" : "nothing");
    return ESP_OK;
}
