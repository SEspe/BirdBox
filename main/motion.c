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
 * Trigger metric: the largest 4-connected cluster of changed cells (§ROI
 * below) must itself cross the sensitivity threshold, not the zone-wide
 * total. A bird is one compact blob; wind-blown grass/foliage is scattered
 * or loosely-connected change spread across many cells — summing the whole
 * zone conflates the two, the dominant-cluster share doesn't.
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
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "jpeg_decoder.h"

static const char *TAG = "motion";

/* 1/8-scale detection frame must fit the largest selectable resolution, which
 * is now camera.c's QSXGA entry (2560x1920 -> 320x240) — the OV5640 ceiling
 * (v2.81). HD (1280x720 -> 160x90) and everything below fit inside it.
 *
 * THIS MUST BE RAISED WHENEVER camera.c's RES TABLE GAINS A LARGER SIZE. These
 * were sized for the old XGA ceiling (128x96); once SXGA/HD were added in v1.21
 * an SXGA frame (160x128 = 20480 px) overran DETECT_MAX_PX and every detect
 * frame was rejected by the px > DETECT_MAX_PX guard below — motion detection
 * silently died at high res, with no error anywhere. Same trap, same fix.
 *
 * Cost is 4 PSRAM buffers (3 grayscale + one RGB565 decode target), 384 KB at
 * this ceiling against 102 KB at the old one. PSRAM idles at ~7.9 MB free, and
 * they are allocated once at startup regardless of the resolution actually
 * selected, so a box running HD pays the QSXGA-sized allocation too — cheap
 * next to making the sizing depend on a setting that needs a reboot anyway. */
#define DETECT_MAX_W      320            /* 1/8 of QSXGA width  (2560) */
#define DETECT_MAX_H      240            /* 1/8 of QSXGA height (1920) */
#define DETECT_MAX_PX     (DETECT_MAX_W * DETECT_MAX_H)
#define DETECT_PERIOD_MS  250
#define PIX_DIFF_THR      25             /* per-pixel gray delta that counts as changed */
#define GLOBAL_STEP_THR   12             /* frame-mean gray shift above this = a global
                                            light/AE step, not local motion: compensate,
                                            and suppress the trigger for that frame */

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
/* DARKNESS IS DECIDED ON CONTRAST, NOT BRIGHTNESS.
 *
 * Measured on the feeder across one day (v3.12), 1/8-scale green channel:
 *
 *   daylight, usable        mean 130-140   std 49-60   max 255
 *   dawn, usable            mean  98       std 52      max 238
 *   dusk, TOO DARK TO USE   mean 148       std 12      max 180
 *   true dark               mean   4       std  0.5    max  11
 *
 * The mean is worthless outdoors: AGC's entire job is to drive it to a target,
 * so it holds near 140 as the light fails and only collapses once the sensor
 * runs out of gain. A dusk frame too dark to photograph measured a HIGHER mean
 * than noon — so a mean threshold can essentially never fire, which is exactly
 * why the box sat "online, bright" through a night it could not see.
 *
 * Contrast does not lie. Amplifying a dark scene amplifies its noise too and
 * leaves the frame flat: 12 against 49-60 for real light, a 4x gap with
 * nothing in between. Stored as VARIANCE to avoid a sqrt per frame, so these
 * are std^2: 25^2 and 35^2.
 *
 * AMBIENT_HIGHLIGHT is the safety net. A flat frame is not always a dark one —
 * a blank wall, fog or snow is low-contrast in full daylight — so a frame is
 * only called dark if it ALSO has no highlights. Real light here always
 * reached 238-255; the unusable dusk frame peaked at 180. */
#define AMBIENT_DARK_ON_VAR   625   /* std < 25: no real detail -> dark     */
#define AMBIENT_DARK_OFF_VAR 1225   /* std > 35: a real scene   -> bright   */
#define AMBIENT_HIGHLIGHT     210   /* peak luma that proves genuine light  */
/* Night probe (FSD §14): frames discarded after a camera wake before the
 * reading is trusted, and the gap between them. ~1 s total, which is enough
 * for the OV2640/OV5640 AEC to converge on the real scene. */
#define AMBIENT_PROBE_MIN_FRAMES    6   /* never trust fewer than ~1.5 s        */
#define AMBIENT_PROBE_MAX_FRAMES   24   /* cap ~6 s, then take what we have     */
#define AMBIENT_PROBE_STABLE_DELTA  3   /* consecutive frames this close = done */
#define AMBIENT_PROBE_SETTLE_MS   250

static uint8_t *s_bg, *s_bg_slow, *s_cur, *s_rgb;
static bool     s_have_bg = false;
static bool     s_dark     = false;   /* hysteresis-debounced ambient state */
static int      s_last_var = 0;       /* variance of the last decoded frame */
static int      s_last_max = 0;       /* peak luma of the last decoded frame */
static bool     s_illum_on = false;
static bool     s_fshut_on = false;
static uint32_t s_cam_gen   = 0;   /* camera_init_generation() we last configured for */
static int      s_px = 0;
static int      s_w = 0, s_h = 0;        /* current detect-frame dimensions */
static roi_t    s_roi;                   /* changed-cell bbox of the last trigger */

static volatile bool     s_motion_active = false;
static volatile uint32_t s_trigger_count = 0;
static volatile int64_t  s_cooldown_until_us = 0;   /* post-event cool-down end (v2.57) */
static volatile uint32_t s_fast_last_ms = 0;        /* last fast-burst avg inter-frame gap */
static volatile uint32_t s_fast_avg_ms  = 0;        /* 4-event EMA of it (Debug, v2.60) */
static volatile uint64_t s_trigger_cells = 0;   /* 8x8 mask of the last trigger's
                                                   winning cluster, for the live-view
                                                   overlay (bit c = cell, §3.1) */
/* Oversized-cluster rejection telemetry (§3.1) — see motion.h. */
static volatile uint32_t s_reject_count  = 0;
static volatile int      s_reject_cells  = 0;
static volatile int      s_cluster_cells = 0;
static volatile bool     s_detect_enabled = true;   /* default on at boot (FSD §5) */
/* Night sleep (FSD §14) pauses detection through its OWN flag rather than
 * s_detect_enabled. Two independent reasons to be paused must not clobber one
 * another: sharing a single flag let night.c and the maintenance toggle each
 * undo the other, and a box was observed stuck with detection off while night
 * sleep believed it was awake — which killed detection and, because the paused
 * loop then grabbed no frames at all, also killed the ambient reading that
 * would have recovered it. */
static volatile bool     s_night_paused   = false;

#define GRID_N 8                         /* 8x8 detection grid (FSD §3.1) */
/* Max cells (of 64) the winning cluster may span before it is rejected as wind
 * or foliage. How much of the grid a bird covers is a property of the MOUNT,
 * not of the bird: at arm's length one fills a third of the frame, at feeder
 * distance a handful of cells. A single fixed cap cannot serve both — too low
 * and a close bird is silently discarded as a wind swath, too high and a
 * distant box logs every gust. g_settings.mount picks it; DISTANT keeps the
 * historical 20 (FSD §3.1). Field note: at the reference feeder, whose camera
 * sits at seed level, one ordinary trigger measured 19 of 64 cells — one under
 * the old cap. */
#define CLUSTER_CAP_CLOSE   40
#define CLUSTER_CAP_MEDIUM  28
#define CLUSTER_CAP_DISTANT 20

static int cluster_cap(void)
{
    switch ((mount_dist_t) g_settings.mount) {
    case MOUNT_CLOSE:   return CLUSTER_CAP_CLOSE;
    case MOUNT_DISTANT: return CLUSTER_CAP_DISTANT;
    default:            return CLUSTER_CAP_MEDIUM;
    }
}

/* Grab one frame, decode small, update s_cur; returns true when the changed
 * area exceeds the sensitivity-derived threshold. Rolls the background only
 * on no-motion frames. */
/* Grab one frame and decode it into s_cur as grayscale, returning the frame's
 * average luma (0-255), or -1 if no frame was available or the decode failed.
 *
 * Factored out of detect_once() so the night probe (FSD §14) can measure
 * ambient light through the IDENTICAL path while the detect loop is paused.
 * That identity is the whole point: the AMBIENT_DARK_*_VAR thresholds are tuned
 * against this exact 1/8-scale, green-channel transform, and a second
 * measurement taken any other way would not be comparable to them. */
static int decode_gray(void)
{
    camera_fb_t *fb = camera_grab();
    if (!fb) return -1;

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
        return -1;
    }

    int px = out.width * out.height;
    if (px <= 0 || px > DETECT_MAX_PX) return -1;
    if (px != s_px) { s_px = px; s_have_bg = false; }
    s_w = out.width;
    s_h = out.height;

    /* Grayscale ≈ green channel of RGB565 (6 bits, scaled to 8) — a stable
     * transform is all differencing needs, not colorimetric accuracy. */
    const uint16_t *rgb = (const uint16_t *) s_rgb;
    long long luma_sum = 0, luma_sq = 0;
    int luma_max = 0;
    for (int i = 0; i < px; i++) {
        int v = ((rgb[i] >> 5) & 0x3F) << 2;
        s_cur[i] = (uint8_t) v;
        luma_sum += v;
        luma_sq  += (long long) v * v;
        if (v > luma_max) luma_max = v;
    }
    int avg = (int) (luma_sum / px);
    /* Variance, not std: comparing against a squared threshold avoids a sqrt
     * on every detect frame. int64 throughout — at the QSXGA ceiling this sums
     * 76 800 squares of up to 65 025, which overflows int32 comfortably. */
    s_last_var = (int) (luma_sq / px - (long long) avg * avg);
    s_last_max = luma_max;
    return avg;
}

/* Feed one luma reading through the dark/bright hysteresis. Shared by the
 * detect loop and the night probe so the ambient state stays coherent no
 * matter which one is currently running (FSD §14). */
static void ambient_update(int avg)
{
    (void) avg;   /* kept for logging/telemetry; it does NOT decide darkness */
    /* CONTRAST decides, not brightness — see the threshold block above for the
     * measurements. Highlights are the safety net: a flat but genuinely lit
     * scene (a blank wall, fog, snow) also has low contrast, and without the
     * max check the box would call that night. Real daylight in this data
     * always reached 238-255; the unusable dusk frame peaked at 180. */
    bool flat   = s_last_var < AMBIENT_DARK_ON_VAR;
    bool nohigh = s_last_max < AMBIENT_HIGHLIGHT;
    bool rich   = s_last_var > AMBIENT_DARK_OFF_VAR;

    if (!s_dark && flat && nohigh)          s_dark = true;
    else if (s_dark && (rich || s_last_max >= AMBIENT_HIGHLIGHT)) s_dark = false;
}

static bool detect_once(void)
{
    int avg = decode_gray();
    if (avg < 0) return false;
    int px = s_px;

    /* Runs on every decoded frame regardless of motion/background state, so
     * both auto behaviours track ambient light continuously rather than only
     * during a visit — "on during dark hours", not a per-shot flash/trigger. */
    ambient_update(avg);

    bool want_illum = (g_settings.ir_led_mode == 1) && s_dark;
    if (want_illum != s_illum_on) { illum_set(want_illum); s_illum_on = want_illum; }

    /* The sensor is reset to a known baseline by every camera_hw_init() — boot,
     * watchdog recovery, and night-sleep wake — and that baseline has fast
     * shutter OFF. A cached "it is already on" therefore describes a sensor
     * that no longer exists, and the compare below would never re-apply it.
     * That is precisely what inverted night detection: after the first wake
     * probe the box metered the night with full auto exposure, read it as
     * bright, and stayed awake all night (FSD §14/v3.08). */
    uint32_t gen = camera_init_generation();
    if (gen != s_cam_gen) {
        s_cam_gen = gen;
        s_fshut_on = false;      /* hw_init forced it off; forget what we cached */
    }
    bool want_fshut = (g_settings.fast_shutter == 1) && s_dark;
    if (want_fshut != s_fshut_on) { camera_set_fast_shutter(want_fshut); s_fshut_on = want_fshut; }

    if (!s_have_bg) {
        memcpy(s_bg, s_cur, px);
        memcpy(s_bg_slow, s_cur, px);
        s_have_bg = true;
        return false;
    }

    /* Global-illumination compensation: a sudden auto-exposure/gain swing (e.g.
     * the moment the live stream opens and the OV2640 re-converges) shifts the
     * WHOLE frame's brightness in one step, faster than the EMA backgrounds
     * follow. Clipping highlights non-uniformly, so the change forms a bird-
     * sized cluster that slips past the 20-cell whole-frame cap and fires a
     * false trigger. Subtract each background's frame-mean delta before
     * thresholding, so a uniform step cancels out while a real (local) mover
     * still stands proud; a large residual mean shift is treated as a lighting
     * event and suppresses the trigger for that frame (see global_step). */
    long dsum_fast = 0, dsum_slow = 0;
    for (int i = 0; i < px; i++) {
        dsum_fast += (int) s_cur[i] - (int) s_bg[i];
        dsum_slow += (int) s_cur[i] - (int) s_bg_slow[i];
    }
    int  shift_fast  = (int) (dsum_fast / px);
    int  shift_slow  = (int) (dsum_slow / px);
    bool global_step = abs(shift_fast) >= GLOBAL_STEP_THR;

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
            /* Changed if it diverges from EITHER background AFTER removing that
             * background's global brightness shift — the fast one (adapts in
             * ~1 s) catches a normal arrival, the slow one (~11 s half-life)
             * still shows a bird that hopped in gradually and got partly
             * absorbed by the fast EMA before ever crossing threshold. */
            if (abs(((int) s_cur[idx] - (int) s_bg[idx])      - shift_fast) > PIX_DIFF_THR ||
                abs(((int) s_cur[idx] - (int) s_bg_slow[idx]) - shift_slow) > PIX_DIFF_THR)
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

    /* Cluster BEFORE deciding motion (moved[] doesn't depend on that decision):
     * bounding box of the LARGEST 4-connected cluster of moved cells (weighted
     * by changed pixels), padded one cell each way for context. Two separate
     * movers (bird + in-zone shadow/feeder swing) no longer merge into one
     * diluted bbox — the zoom tracks the dominant object. */
    bool seen[GRID_N * GRID_N] = {false};
    long best_wt = -1;
    const int cap = cluster_cap();   /* mount-dependent (§3.1) */
    int  best_cnt = 0, rej_max = 0;  /* winning / largest-rejected cluster size */
    uint64_t best_cells = 0;    /* moved-cell mask of the winning cluster */
    int  minc = 0, minr = 0, maxc = -1, maxr = -1;
    for (int c0 = 0; c0 < GRID_N * GRID_N; c0++) {
        if (!moved[c0] || seen[c0]) continue;
        int  stack[GRID_N * GRID_N], sp = 0;
        long wt  = 0;
        int  cnt = 0;
        uint64_t this_cells = 0;
        int  lminc = GRID_N, lminr = GRID_N, lmaxc = -1, lmaxr = -1;
        stack[sp++] = c0;
        seen[c0] = true;
        while (sp > 0) {
            int c  = stack[--sp];
            int cc = c % GRID_N, cr = c / GRID_N;
            wt += cell_changed[c];
            this_cells |= (1ULL << c);
            cnt++;
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
        /* Oversized clusters are skipped entirely, not just capped — a
         * smaller genuine cluster elsewhere in the same frame can still win
         * rather than the whole frame being rejected outright. The largest
         * rejected size is kept so the quiet path can SAY it happened: an
         * oversized cluster used to fail silently, which is exactly how a
         * close-mounted camera loses every bird without a trace. */
        if (cnt > cap) {
            if (cnt > rej_max) rej_max = cnt;
        } else if (wt > best_wt) {
            best_wt = wt;
            best_cells = this_cells;
            best_cnt = cnt;
            minc = lminc; minr = lminr; maxc = lmaxc; maxr = lmaxr;
        }
    }
    if (maxc >= minc && maxr >= minr && best_wt >= 0) {
        /* Refine the ROI to the tight bounding box of the actually-changed PIXELS
         * inside the winning cluster's cells (v2.31). The 160x90 diff we already
         * computed has far finer detail than the 1/8 cell grid, so the crop handed
         * to species ID hugs the bird instead of snapping to 12.5% cell lines —
         * more reliable than a finer detection grid (too few px/cell) and free.
         * Falls back to the padded cell bbox if somehow no pixel is flagged. */
        int pminx = W, pminy = H, pmaxx = -1, pmaxy = -1;
        for (int y = 0; y < H; y++) {
            const int rb = (y * GRID_N / H) * GRID_N;
            for (int x = 0; x < W; x++) {
                if (!(best_cells & (1ULL << (rb + x * GRID_N / W)))) continue;
                int idx = y * W + x;
                if (abs(((int) s_cur[idx] - (int) s_bg[idx])      - shift_fast) > PIX_DIFF_THR ||
                    abs(((int) s_cur[idx] - (int) s_bg_slow[idx]) - shift_slow) > PIX_DIFF_THR) {
                    if (x < pminx) pminx = x;
                    if (x > pmaxx) pmaxx = x;
                    if (y < pminy) pminy = y;
                    if (y > pmaxy) pmaxy = y;
                }
            }
        }
        if (pmaxx >= pminx && pmaxy >= pminy) {
            pminx = pminx > 1     ? pminx - 2 : 0;      /* 2px safety pad; the crop */
            pminy = pminy > 1     ? pminy - 2 : 0;      /* adds 1.4x context on top */
            pmaxx = pmaxx < W - 2 ? pmaxx + 2 : W - 1;
            pmaxy = pmaxy < H - 2 ? pmaxy + 2 : H - 1;
            s_roi.x0 = (float) pminx / W;
            s_roi.y0 = (float) pminy / H;
            s_roi.x1 = (float) (pmaxx + 1) / W;
            s_roi.y1 = (float) (pmaxy + 1) / H;
        } else {
            if (--minc < 0) minc = 0;
            if (--minr < 0) minr = 0;
            if (++maxc > GRID_N - 1) maxc = GRID_N - 1;
            if (++maxr > GRID_N - 1) maxr = GRID_N - 1;
            s_roi.x0 = (float) minc / GRID_N;
            s_roi.y0 = (float) minr / GRID_N;
            s_roi.x1 = (float) (maxc + 1) / GRID_N;
            s_roi.y1 = (float) (maxr + 1) / GRID_N;
        }
    } else {
        s_roi = roi_none();
    }

    /* Trigger on the DOMINANT cluster's share of the zone, not the raw
     * zone-wide %. Wind-blown grass/foliage tends to change many scattered
     * or loosely-connected cells a little each; a bird changes one compact
     * blob a lot. Using the largest connected cluster's weight instead of
     * the sum over all cells rejects the former without needing a manual
     * zone-mask exclusion for every wind-prone patch of grass. */
    int  pct         = (int) (zone_changed * 100 / zone_px);
    int  cluster_pct = best_wt > 0 ? (int) (best_wt * 100 / zone_px) : 0;
    /* A frame dominated by a global light step never triggers, even if the
     * clipping residual still forms a cluster — the EMA rolls below re-seed
     * the background so detection recovers cleanly within ~1 s. */
    bool motion      = cluster_pct >= area_thr && !global_step;

    if (global_step && cluster_pct >= area_thr)
        ESP_LOGI(TAG, "motion suppressed: global light step (frame-mean shift %d)", shift_fast);

    if (!motion) {
        /* Say so when the ONLY thing in frame was thrown out for being too
         * wide — throttled to once per 5 s so a long wind swath cannot flood
         * the log. This is the line that tells you to move the mount setting
         * up rather than chasing sensitivity, which only makes clusters bigger. */
        if (rej_max > 0) { s_reject_count++; s_reject_cells = rej_max; }
        if (rej_max > 0 && best_wt < 0) {
            static int64_t last_rej_us = 0;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_rej_us > 5000000) {
                last_rej_us = now_us;
                ESP_LOGI(TAG, "cluster rejected: %d cells > cap %d (mount=%u) — "
                              "raise the mount distance setting if birds are being missed",
                         rej_max, cap, (unsigned) g_settings.mount);
            }
        }
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

    s_trigger_cells = best_cells;   /* publish which cells fired, for the live overlay */
    s_cluster_cells = best_cnt;
    ESP_LOGI(TAG, "motion: cluster %d%% / zone %d%% changed (threshold %d%%), %d/%d cells, roi [%.2f,%.2f]-[%.2f,%.2f]",
             cluster_pct, pct, area_thr, best_cnt, cap, s_roi.x0, s_roi.y0, s_roi.x1, s_roi.y1);
    return true;
}

static void capture_event(roi_t roi)
{
    char  first_path[96] = "";
    int   frames = 0;
    int   fast   = 0;
    roi_t cur = roi;   /* trigger-time ROI for frame 0 */

    /* Fast burst (v2.56): FAST_BURST_N frames grabbed back-to-back right at the
     * trigger to catch a fast, short-visit bird the 1 s slow cadence would miss
     * (e.g. a Granmeis that stops for under a second). Saved on every event but
     * scored ONLY if the slow frames fail to classify (classify.cpp pools them in
     * as a fallback). No per-frame re-detect, no early-out, and (v2.60) no added
     * delay — grabs are as fast as the sensor allows; the OV2640 frame rate is the
     * floor. They occupy the first `fast` slots of the event's frame arrays. We
     * time the burst so the Debug panel can show the true average gap. */
    int64_t fast_first_us = 0, fast_last_us = 0;
    for (int i = 0; i < FAST_BURST_N; i++) {
        camera_fb_t *fb = camera_grab();
        if (fb) {
            esp_err_t err = capture_event_frame(fb->buf, fb->len, roi,
                                                frames == 0 ? first_path : NULL,
                                                sizeof(first_path));
            camera_return(fb);
            if (err == ESP_OK) {
                int64_t now = esp_timer_get_time();
                if (fast == 0) fast_first_us = now;
                fast_last_us = now;
                frames++; fast++;
            }
            else if (frames == 0) break;   /* no SD — don't spin on a dead write path */
        }
        if (FAST_BURST_MS && i + 1 < FAST_BURST_N) vTaskDelay(pdMS_TO_TICKS(FAST_BURST_MS));
    }
    /* Average inter-frame gap of this burst, plus a 4-event moving average, for
     * the Debug panel (v2.60) — the real-world fast-frame speed, so the sensor
     * frame-rate floor is visible rather than assumed. */
    if (fast >= 2) {
        uint32_t avg = (uint32_t) ((fast_last_us - fast_first_us) / 1000 / (fast - 1));
        s_fast_last_ms = avg;
        s_fast_avg_ms  = s_fast_avg_ms ? (uint32_t) ((s_fast_avg_ms * 3 + avg) / 4) : avg;
    }

    /* Normal slow frames. first_path already anchors the event on the first fast
     * frame (or the first slow frame if every fast grab failed). */
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

    capture_event_finish(frames, fast, first_path);
}

static void motion_task(void *arg)
{
    bool quarantine_logged = false;
    for (;;) {
        if (!s_detect_enabled || s_night_paused) {
            /* Paused: drop the baseline so detection re-seeds against the
             * current scene when it resumes (light/subject may have changed).
             *
             * BUT KEEP SAMPLING AMBIENT LIGHT. A pause used to skip the frame
             * grab entirely, which silently blinded the dark/bright reading —
             * and night sleep (FSD §14) depends on exactly that reading to know
             * when to wake. A box left paused therefore froze its ambient state
             * and could never notice dawn, or dusk. The sample is one small
             * decode, it drives nothing but the illuminator/fast-shutter/night
             * state, and when the sensor is deliberately powered down
             * decode_gray() simply returns -1 and this costs nothing. */
            /* EXACTLY ONE TASK MAY DECODE AT A TIME. decode_gray() writes the
             * shared s_rgb/s_cur buffers and mutates s_px, so two callers at
             * once corrupt both frames and, eventually, the heap. While
             * NIGHT-paused the night task owns the measurement (its wake
             * probe), so this loop must stay out of the way; while merely
             * maintenance-paused nobody else samples, so it samples here. That
             * split is what keeps the pause from blinding the ambient reading
             * without racing the probe for the decoder. */
            if (!s_night_paused) {
                int amb = decode_gray();
                if (amb >= 0) ambient_update(amb);
            }
            s_have_bg = false;
            vTaskDelay(pdMS_TO_TICKS(DETECT_PERIOD_MS));
            continue;
        }
        /* Boot quarantine (§3.1/v1.61): for the first detect_quarantine_s
         * seconds of uptime, don't trigger — the OV2640's auto-exposure/gain
         * settle over the first frames (their swings read as motion), and the
         * clock hasn't SNTP-synced yet, so any event would file under /no-date.
         * Keep re-seeding the baseline so detection starts clean when it lifts. */
        if (g_settings.detect_quarantine_s &&
            esp_timer_get_time() < (int64_t) g_settings.detect_quarantine_s * 1000000) {
            if (!quarantine_logged) {
                ESP_LOGI(TAG, "detection quarantined for %us after boot",
                         g_settings.detect_quarantine_s);
                quarantine_logged = true;
            }
            s_have_bg = false;
            vTaskDelay(pdMS_TO_TICKS(DETECT_PERIOD_MS));
            continue;
        }
        if (detect_once()) {
            s_motion_active = true;
            s_trigger_count++;
            capture_event(s_roi);   /* snapshot the trigger ROI for species ID */
            s_motion_active = false;
            /* Publish the cool-down end so the live view can show a countdown
             * (v2.57); the delay itself is unchanged. */
            s_cooldown_until_us = esp_timer_get_time() + (int64_t) g_settings.cooldown_s * 1000000;
            vTaskDelay(pdMS_TO_TICKS((uint32_t) g_settings.cooldown_s * 1000));
            s_cooldown_until_us = 0;
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
uint64_t motion_trigger_cells(void) { return s_trigger_cells; }

uint16_t motion_quarantine_remaining_s(void)
{
    if (!g_settings.detect_quarantine_s) return 0;
    int64_t up_s = esp_timer_get_time() / 1000000;
    return up_s < g_settings.detect_quarantine_s
         ? (uint16_t) (g_settings.detect_quarantine_s - up_s) : 0;
}

/* Seconds left in the post-event cool-down, 0 when not cooling down (v2.57). */
uint16_t motion_cooldown_remaining_s(void)
{
    int64_t rem = s_cooldown_until_us - esp_timer_get_time();
    return rem > 0 ? (uint16_t) ((rem + 999999) / 1000000) : 0;
}
uint32_t motion_reject_count(void)              { return s_reject_count; }
int      motion_reject_cells(void)              { return s_reject_cells; }
int      motion_cluster_cells(void)             { return s_cluster_cells; }
int      motion_cluster_cap(void)               { return cluster_cap(); }

bool motion_detection_enabled(void)            { return s_detect_enabled; }
void motion_set_detection_enabled(bool enabled) { s_detect_enabled = enabled; }

bool motion_night_paused(void) { return s_night_paused; }

void motion_set_night_paused(bool paused)
{
    s_night_paused = paused;
    /* Switch the illuminator off on the way into a night pause. It is driven
     * ONLY from detect_once(), which does not run while paused, so without this
     * it stays lit all night over an unwatched scene with the camera powered
     * down — burning current and making heat to light nothing, which is one of
     * the exact wastes night sleep exists to stop (FSD §14).
     *
     * Done HERE, inside the module that owns the illuminator, rather than from
     * night.c: clearing s_illum_on alongside the hardware keeps belief and
     * reality in step. A caller reaching around this cache would recreate the
     * stale-cache failure v3.08 had to fix for fast shutter — the flag would
     * still say "on", so detect_once() would never re-apply it and the
     * illuminator would stay dark on the night it was actually wanted.
     * Resuming needs nothing: detect_once() re-evaluates on its next frame. */
    if (paused && s_illum_on) {
        illum_set(false);
        s_illum_on = false;
    }
}

bool motion_ambient_dark(void) { return s_dark; }

/* The numbers that actually DECIDE darkness, exposed so the reason is visible.
 * Without these, /api/night showing luma 148 next to dark:true reads as a
 * contradiction — the mean is now only a display value (see the threshold
 * block: it stays ~140 as the light fails, which is the whole problem). */
int motion_ambient_contrast(void) { return (int) sqrtf((float) (s_last_var > 0 ? s_last_var : 0)); }
int motion_ambient_peak(void)     { return s_last_max; }

/* One-shot ambient measurement for the night probe (FSD §14), used while the
 * detect loop is paused and therefore not producing readings of its own.
 *
 * Discards warm-up frames. The sensor's AEC/AGC swing wildly over the first
 * frames after a wake, and a sample taken immediately reads far darker than
 * the scene really is — which would keep a box asleep through sunrise. This is
 * the same settling the boot detection quarantine exists for; here it costs
 * four frames instead of sixty seconds because nothing is being captured.
 *
 * Returns the average luma 0-255, or -1 if no frame could be decoded. */
int motion_ambient_probe(void)
{
    /* Settle until the reading CONVERGES, not for a fixed time. A freshly woken
     * sensor's AEC/AGC is still hunting, and a fixed ~1 s was measured to
     * under-read badly: at dawn a probe reported luma 70 while the settled
     * detector, same scene and same minute, reported 105 — either side of the
     * bright threshold, so the box stayed asleep in daylight. Stop as soon as
     * two consecutive frames agree closely, and cap the wait so a flickering
     * scene cannot hold the camera on. */
    int avg = -1, prev = -1, stable = 0;
    for (int i = 0; i < AMBIENT_PROBE_MAX_FRAMES; i++) {
        vTaskDelay(pdMS_TO_TICKS(AMBIENT_PROBE_SETTLE_MS));
        avg = decode_gray();
        if (avg < 0) continue;
        if (prev >= 0 && abs(avg - prev) <= AMBIENT_PROBE_STABLE_DELTA) {
            if (++stable >= 2 && i + 1 >= AMBIENT_PROBE_MIN_FRAMES) break;
        } else {
            stable = 0;
        }
        prev = avg;
    }
    if (avg >= 0) ambient_update(avg);
    /* The scene almost certainly changed while the camera was off, so never
     * let a night-old background survive into the resumed detector. */
    s_have_bg = false;
    return avg;
}

uint32_t motion_fast_last_ms(void) { return s_fast_last_ms; }   /* Debug (v2.60) */
uint32_t motion_fast_avg_ms(void)  { return s_fast_avg_ms; }
