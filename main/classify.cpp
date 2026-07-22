/* Species identification orchestrator (FSD §3.2/§3.2.3).
 *
 * As of v2.36 there is NO on-device model — the TFLite-Micro nordic classifier
 * was removed (freeing flash, the ~3 MB PSRAM arena, and the SD model files).
 * Classification is online only: iNaturalist CV (primary, inat.c) with a >=2-
 * frame corroboration + geo-filter + best-of-crop, and an optional cloud tier
 * (Claude/Gemini, cloud.c) as fallback and for the manual ✨ button.
 *
 * Pipeline: motion.c saves an event's frames; capture_event_finish() queues them
 * here and returns immediately — capture is never delayed. The async event task
 * scores the frames via iNat (paced HTTPS), decides the label, and writes the
 * visit-log row. The only image work left on-device is classify_crop_jpeg (decode
 * + native-res ROI crop) to hand iNat a subject-filling frame. */
#include "classify.h"
#include "cloud.h"          /* has its own extern "C" guard */
#include "inat.h"           /* iNaturalist online CV (primary tier) */
#include "species_i18n.h"   /* has its own extern "C" guard */
extern "C" {          /* C headers without their own __cplusplus guards */
#include "settings.h"
#include "storage.h"
#include "target_species.h"
}

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>          /* unlink — was transitively via the removed TFLM headers */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "img_converters.h"   /* fmt2jpg — re-encode a native-res ROI crop for iNat */

static const char *TAG = "classify";

static volatile int32_t s_last_ms = -1;
static char             s_last_species[64] = "";
static char             s_last_latin[64] = "";
static uint8_t          s_last_conf = 0;      /* confidence % of the last event */
static char             s_last_file[96] = "";  /* frame that scored the last ID's peak —
                                                  the "best image" the live-view Last ID
                                                  badge links to (v2.53) */
static volatile bool    s_last_event_ided = false; /* did the MOST RECENT event get a real
                                                       species? gates the live badge (v2.29) */
static volatile bool    s_cls_busy = false;  /* a job is being scored right now — drives the
                                                live view's yellow CLASSIFYING state (v2.54).
                                                Set while the task holds a job, so it spans the
                                                whole iNat round-trip, not just the queue. */
static volatile bool    s_fast_active = false; /* true while the fast-burst backup frames are
                                                  being scored (slow frames failed) — the live
                                                  view's FASTBIRD DETECTION state (v2.56). */
static volatile uint32_t s_cls_seq = 0;   /* ++ when an event's async classification RESULT
                                             lands (v2.40). The live-view "Current" badge keys
                                             on this, not the capture-time event count — the
                                             result arrives seconds after capture. */

#if CONFIG_IDF_TARGET_ESP32S3

/* RGB888 decode cap for the ROI crop (classify_crop_jpeg). Kept at 1.5 MB so an
 * HD frame halves to 640x360 for the crop. A v2.39 experiment lifted this to
 * 3 MB (full-res decode → bigger crop) on the theory that more pixels help iNat;
 * MEASURED to NOT help — iNat's score_image resizes every upload to ~299² anyway,
 * so a 360² crop already saturates its input, and the larger crop only forced a
 * so a 360² crop already saturates its input, and the larger crop only forced a
 * lower JPEG quality (and iNat 500s on a big high-quality crop). Re-tested 0.74.4
 * on a small corner-bird box: full-res bumped it 4→7% but still Unidentified/wrong
 * species — no useful gain. Don't re-raise. */
#define CLS_DECODE_MAX   (1536 * 1024)

typedef struct {
    char     ts[24];
    int      frames;
    char     first_path[96];
    char     paths[CLASSIFY_BEST_OF_N][96];  /* saved-frame paths to score */
    roi_t    rois[CLASSIFY_BEST_OF_N];       /* per-frame motion zoom (§3.1) */
    int      path_count;
    int      fast_count;                     /* the first `fast_count` of paths[] are the
                                                fast-burst backup, scored only on fallback
                                                (v2.56); the rest are the normal slow frames */
} cls_job_t;
static QueueHandle_t s_jobq = NULL;

/* ── Model / label loading ──────────────────────────────────────────────── */
static uint8_t *load_file_psram(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *) heap_caps_malloc(st.st_size + 1,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (got != (size_t) st.st_size) { free(buf); return NULL; }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Crop a square centred on `roi` (expanded ~1.4x for context) at the NATIVE
 * decode resolution — NOT the 224² model input — and re-encode as JPEG. For the
 * online iNat tier: a small/off-centre bird then FILLS the frame while staying
 * sharp, unlike the reverted v2.22 crop that downscaled to 224² and measured
 * worse. Decode is capped at CLS_DECODE_MAX (so HD halves to 640x360), which
 * bounds the crop's absolute pixels. Rotation is not applied (saved frames are
 * in native camera orientation). Caller frees *out_jpg. (v2.30, test path.) */
esp_err_t classify_crop_jpeg(const uint8_t *jpeg, size_t len, roi_t roi,
                             uint8_t **out_jpg, size_t *out_len)
{
    if (roi_is_empty(roi) || !out_jpg || !out_len) return ESP_ERR_INVALID_ARG;

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = (uint8_t *) jpeg; cfg.indata_size = len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    esp_jpeg_image_output_t info = {};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || !info.width || !info.height)
        return ESP_FAIL;

    int scale = 0, w = info.width, h = info.height;
    while (scale < 3 && (size_t) w * h * 3 > CLS_DECODE_MAX) { w >>= 1; h >>= 1; scale++; }
    if ((size_t) w * h * 3 > CLS_DECODE_MAX) return ESP_ERR_NO_MEM;

    size_t   rgb_sz = (size_t) w * h * 3 + 16;
    uint8_t *rgb = (uint8_t *) heap_caps_malloc(rgb_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) return ESP_ERR_NO_MEM;
    cfg.outbuf = rgb; cfg.outbuf_size = rgb_sz; cfg.out_scale = (esp_jpeg_image_scale_t) scale;
    esp_jpeg_image_output_t out = {};
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) { free(rgb); return ESP_FAIL; }
    w = out.width; h = out.height;

    int rx0 = (int) (roi.x0 * w + 0.5f), ry0 = (int) (roi.y0 * h + 0.5f);
    int rw  = (int) ((roi.x1 - roi.x0) * w + 0.5f);
    int rh  = (int) ((roi.y1 - roi.y0) * h + 0.5f);
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rw < 1)  rw = 1;
    if (rh < 1)  rh = 1;
    int side = (rw > rh ? rw : rh);
    side = (int) (side * 1.4f);            /* pad the box with context */
    int fmin = (w < h ? w : h);
    if (side > fmin) side = fmin;
    int x0 = rx0 + rw / 2 - side / 2, y0 = ry0 + rh / 2 - side / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + side > w) x0 = w - side;
    if (y0 + side > h) y0 = h - side;

    uint8_t *crop = (uint8_t *) heap_caps_malloc((size_t) side * side * 3,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!crop) { free(rgb); return ESP_ERR_NO_MEM; }
    for (int y = 0; y < side; y++)
        memcpy(crop + (size_t) y * side * 3,
               rgb + ((size_t) (y0 + y) * w + x0) * 3, (size_t) side * 3);
    free(rgb);

    bool ok = fmt2jpg(crop, (size_t) side * side * 3, side, side,
                      PIXFORMAT_RGB888, 90, out_jpg, out_len);
    free(crop);
    ESP_LOGI(TAG, "crop %dx%d from roi [%.2f,%.2f]-[%.2f,%.2f] -> %s (%u B)",
             side, side, roi.x0, roi.y0, roi.x1, roi.y1,
             ok ? "ok" : "encode-failed", ok ? (unsigned) *out_len : 0);
    return ok ? ESP_OK : ESP_FAIL;
}

/* best-of-N ranking: a decided real species (non-empty latin — the "no bird"/
 * "Unidentified bird"/"unclassified" sentinels leave it empty) always beats a
 * sentinel, so a confident background frame can't outrank a real ID from
 * another frame; among equals, higher confidence wins. */
static bool result_better(const classify_result_t *n, const classify_result_t *c)
{
    bool nr = n->latin[0] != '\0', cr = c->latin[0] != '\0';
    if (nr != cr) return nr;
    if (!nr) {
        /* Both are sentinels. "Unidentified bird" means a bird WAS seen; "no bird"
         * means none was. Those aren't the same currency, so they must never be
         * compared by confidence: a whole frame calling background at 15% would
         * otherwise beat a crop that actually found a Dompap at 11% and the visit
         * would be filed as background (v2.46 — measured on three real events).
         * Bird evidence always wins, however low its score. */
        bool n_nobird = strcmp(n->species, "no bird") == 0;
        bool c_nobird = strcmp(c->species, "no bird") == 0;
        if (n_nobird != c_nobird) return c_nobird;   /* better only if it replaces a no-bird */
    }
    return n->confidence_pct > c->confidence_pct;
}

/* top-3 as a compact CSV-safe field for the visit log (§3.4 tuning data):
 * "Latin=pct;Latin=pct;Latin=pct" — Latin binomial only (labels' common-name
 * halves would bloat the row), commas sanitized to ';'. */
static void top3_field(const classify_result_t *r, char *out, size_t n)
{
    size_t off = 0;
    out[0] = '\0';
    for (int k = 0; k < 3 && off + 8 < n; k++) {
        if (!r->top_label[k][0] || r->top_pct[k] == 0) break;
        char latin[64];
        const char *open = strrchr(r->top_label[k], '(');
        size_t l = open ? (size_t) (open - r->top_label[k]) : strlen(r->top_label[k]);
        while (l > 0 && r->top_label[k][l - 1] == ' ') l--;
        if (l >= sizeof(latin)) l = sizeof(latin) - 1;
        memcpy(latin, r->top_label[k], l);
        latin[l] = '\0';
        for (char *c = latin; *c; c++)
            if (*c == ',') *c = ';';
        off += snprintf(out + off, n - off, "%s%s=%u", k ? ";" : "", latin, r->top_pct[k]);
    }
}

/* Identify one event with Claude instead of the local model (§3.2.3).
 *
 * One call per EVENT, not per frame: best-of-N exists to paper over the local
 * model's pose sensitivity, which Claude does not share, so scoring all three
 * frames would triple the bill for a marginal gain. The event's first frame is
 * the one motion.c already judged best.
 *
 * The whole frame goes up regardless of detect_zoom — a cloud model reasons
 * about context (perch, feeder, other birds) and a tight crop throws that away,
 * the same effect that made zoom hurt the iNat model (§3.2.1/v1.87).
 *
 * Uses whichever single cloud provider is active (Claude or Gemini — cloud.c).
 * Returns false on any failure — none active, WiFi down, API error — and the
 * caller falls back to the on-device model rather than dropping the event. */
static bool cloud_event(const cls_job_t *job, classify_result_t *out, roi_t *win)
{
    cloud_provider_t p = cloud_active();
    if (p == CLOUD_OFF) return false;

    char full[128];
    snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job->paths[0]);
    if (cloud_classify_file(p, full, out) != ESP_OK) {
        ESP_LOGW(TAG, "%s failed (%s) — falling back to the on-device model",
                 cloud_name(p), cloud_last_error(p));
        return false;
    }
    *win = job->rois[0];   /* logged as field data either way (§3.4) */
    return true;
}

/* iNaturalist online CV — the PRIMARY tier when enabled. Sends the WHOLE frame
 * (like the cloud LLM tiers). An ROI-crop variant was tried (v2.22) on the
 * theory that iNat wants a subject-filling photo, but it measured WORSE on real
 * feeder birds — the same Bokfink scored 20% (unidentified) on the 224² crop vs
 * 93% on the full-res frame (the small nearest-neighbour crop degrades quality,
 * and the motion box isn't always a clean bird box). So whole-frame it is
 * (v2.23). Returns true ONLY on a confident bird species (non-empty latin); an
 * unsure/low-score result yields empty latin → false, so the event falls through
 * to the nordic model (which also owns the "no bird" call — iNat has no reliable
 * empty-frame detection). Any failure returns false too, so a hiccup never drops
 * the event. */
/* A single confident frame may stand alone (skip the >=2-frame corroboration)
 * only at/above this confidence. 75 catches an obvious lone bird that didn't
 * linger (an 81% Dompap / 78% Bokfink were being dropped at 90) while staying
 * ABOVE the 47-72% band where iNat flip-flops between look-alikes (the magpie
 * Pica pica/hudsonia case the guard targets), so those still need a 2nd frame.
 * (v2.33, lowered 90→75 v2.34.) */
#define INAT_SOLO_ACCEPT_PCT 75

/* Score one frame with iNat, best-of whole-vs-crop (v2.31). Always scores the
 * whole frame; when detect_zoom is on and the frame has a motion ROI, also scores
 * a native-res crop centred on the bird (classify_crop_jpeg) and keeps the more
 * confident real species (result_better). A small/off-centre bird that under-
 * scores whole often clears threshold cropped (measured: Bokfink 17→34%, magpie
 * 35→82%), while a messy crop can't drag the result below the whole-frame score.
 * The crop call fires ONLY when the whole frame couldn't identify the bird
 * (empty latin) — that's where it helps, and it keeps a confident whole ID as-is,
 * so a feeder of easy birds pays ~no extra cost and only the hard frames get the
 * second (paced/cooled) iNat call. */
static esp_err_t score_frame_best(const char *full, roi_t roi, classify_result_t *r)
{
    size_t len = 0;
    uint8_t *buf = load_file_psram(full, &len);
    if (!buf) return ESP_FAIL;

    esp_err_t e = inat_classify_jpeg(buf, len, r);
    if (e == ESP_OK && r->latin[0] == '\0' &&
        g_settings.detect_zoom && !roi_is_empty(roi)) {
        uint8_t *cj = NULL; size_t cl = 0;
        if (classify_crop_jpeg(buf, len, roi, &cj, &cl) == ESP_OK && cj) {
            classify_result_t rc;
            if (inat_classify_jpeg(cj, cl, &rc) == ESP_OK && result_better(&rc, r)) {
                ESP_LOGI(TAG, "crop beat whole: %s %u%% > %s %u%%",
                         rc.species, rc.confidence_pct, r->species, r->confidence_pct);
                *r = rc;
            }
            free(cj);
        }
    }
    free(buf);
    return e;
}

/* iNaturalist tier over EVERY saved frame, with a corroboration guard (v2.26).
 * Score all of the event's frames and require the winning species to be seconded
 * by at least TWO frames: the model can be confidently wrong on one view (a bird
 * half-out of frame, an off-pose blur) but rarely on two independent ones, so a
 * lone-frame hit — the false-positive class the single-best path let through —
 * is discarded. Among the species that clear that ">=2 frames agree" bar, the
 * most confident wins ("best seconded species"), logged from its strongest frame.
 * If none is seconded we DECLINE (return false): with the nordic tier off (iNat-
 * sole-source) that lands the event on Unidentified — its own row, or cloud when
 * enabled — i.e. the "else Unidentified" fallback; with nordic on it still gets a
 * shot. Corroboration needs >=2 frames of evidence, so a 1-frame event can never
 * clear it and always declines — acceptable given capture_count defaults to 5.
 * iNat's ~1 req/s pacing and 429 cooldown live inside inat_classify_* (v2.25),
 * so this per-frame loop self-throttles and a cooldown fails every frame cleanly
 * (scored==0 → decline), never hammering a throttled endpoint. `scored_out` gets
 * the winning species' vote count for the visit-log "N/M frame(s)" line. */
static bool inat_event(const cls_job_t *job, classify_result_t *out, roi_t *win,
                       int *scored_out, char *pf, size_t pf_len, char *best_out = NULL,
                       size_t best_len = 0)
{
    if (!inat_cv_enabled()) return false;
    if (pf && pf_len) pf[0] = '\0';       /* per-frame scores for the visit log (v2.29) */
    size_t pfo = 0;

    /* Per distinct species (latin binomial): how many frames voted for it, and
     * the single most-confident frame's full result + ROI — what we'd log. Held
     * in PSRAM: an inat_classify_file() call runs a TLS handshake deep on this
     * task's stack, so the candidate table must not also sit on it. */
    typedef struct { char latin[64]; int votes; uint8_t peak;
                     classify_result_t res; roi_t roi;
                     char best[96]; } inat_cand_t;   /* frame that scored `peak` (v2.53) */
    inat_cand_t *cand = (inat_cand_t *) heap_caps_malloc(
        (size_t) job->path_count * sizeof(inat_cand_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cand) {
        /* OOM → degrade to the pre-v2.26 single-best-frame path (frame 0). */
        char full[128];
        snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job->paths[0]);
        if (inat_classify_file(full, out) != ESP_OK || out->latin[0] == '\0')
            return false;
        *win = job->rois[0];
        if (scored_out) *scored_out = 1;
        return true;
    }

    int ncand = 0, scored = 0, nobird = 0, unid = 0;
    classify_result_t nobird_res; bool have_nobird = false;

    /* Score frames [lo, hi) into the shared candidate table. Split out so the
     * slow frames can be scored first and the fast-burst backup only added on
     * fallback (v2.56); with fast_count 0 (recheck) this scores every frame in
     * one pass, unchanged. */
    auto score_range = [&](int lo, int hi) {
        for (int i = lo; i < hi; i++) {
            char full[128];
            snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job->paths[i]);
            classify_result_t r;
            esp_err_t e = score_frame_best(full, job->rois[i], &r);

            /* Per-frame visit-log column: "<binomial>=<pct>" for a decided
             * species, "<binomial>?=<pct>" when unsure (binomial from raw top-1),
             * or "err" on a failed call (v2.29). Fallback events append the fast
             * frames' scores after the slow ones. */
            if (pf && pfo + 48 < pf_len) {
                if (e != ESP_OK) {
                    pfo += snprintf(pf + pfo, pf_len - pfo, "%serr", pfo ? ";" : "");
                } else {
                    char lat[48] = "";
                    if (r.latin[0]) {
                        strlcpy(lat, r.latin, sizeof(lat));
                    } else {                    /* binomial from the raw top-1 label */
                        const char *op = strchr(r.top_label[0], '(');
                        size_t ll = op ? (size_t)(op - r.top_label[0]) : strlen(r.top_label[0]);
                        while (ll && r.top_label[0][ll - 1] == ' ') ll--;
                        if (ll >= sizeof(lat)) ll = sizeof(lat) - 1;
                        memcpy(lat, r.top_label[0], ll); lat[ll] = '\0';
                    }
                    pfo += snprintf(pf + pfo, pf_len - pfo, "%s%s%s=%u",
                                    pfo ? ";" : "", lat, r.latin[0] ? "" : "?",
                                    r.confidence_pct);
                }
            }

            if (e != ESP_OK) {
                ESP_LOGW(TAG, "iNat CV frame %d failed (%s)", i, inat_last_error());
                continue;                       /* 429 cooldown fails all → scored 0 */
            }
            scored++;
            if (r.latin[0] == '\0') {           /* no species from this frame — but which kind? */
                if (strcmp(r.species, "no bird") == 0) {   /* iNat's top guess was non-Aves */
                    nobird++;
                    if (!have_nobird) { nobird_res = r; have_nobird = true; }
                } else {
                    unid++;                        /* a bird was seen, just not identifiable */
                }
                continue;
            }

            int slot = -1;
            for (int k = 0; k < ncand; k++)
                if (strcmp(cand[k].latin, r.latin) == 0) { slot = k; break; }
            if (slot < 0) {
                slot = ncand++;
                strlcpy(cand[slot].latin, r.latin, sizeof(cand[slot].latin));
                cand[slot].votes = 0;
                cand[slot].peak  = 0;
            }
            cand[slot].votes++;
            if (r.confidence_pct >= cand[slot].peak) {   /* keep the strongest view */
                cand[slot].peak = r.confidence_pct;
                cand[slot].res  = r;
                cand[slot].roi  = job->rois[i];
                strlcpy(cand[slot].best, job->paths[i], sizeof(cand[slot].best));
            }
        }
    };

    /* Winner = most-confident species seconded by >=2 frames; else a lone frame
     * only if VERY confident (>= INAT_SOLO_ACCEPT_PCT, v2.33) so an obvious bird
     * that didn't linger isn't dropped, while a weak lone hit still needs a
     * second frame (the false-positive guard). */
    auto pick_winner = [&]() -> int {
        int w = -1;
        for (int k = 0; k < ncand; k++)
            if (cand[k].votes >= 2 && (w < 0 || cand[k].peak > cand[w].peak)) w = k;
        if (w < 0)
            for (int k = 0; k < ncand; k++)
                if (cand[k].peak >= INAT_SOLO_ACCEPT_PCT &&
                    (w < 0 || cand[k].peak > cand[w].peak)) w = k;
        return w;
    };

    /* Slow frames first. The fast-burst backup ([0, fast_count)) is scored — and
     * uploaded to iNat — ONLY if the slow frames yield no winner (v2.56), so the
     * common case pays no extra iNat calls, latency, or heap. On fallback the
     * fast frames are POOLED with the slow ones (their scores are already in
     * cand[]), so a fast frame can corroborate a weak slow frame. */
    score_range(job->fast_count, job->path_count);
    int win_k = pick_winner();
    /* Fall back to the fast burst ONLY if the slow frames actually SAW a bird —
     * a candidate species short of the winner bar (ncand), or an unidentified
     * Aves (unid). If every slow frame was background/no-bird (ncand==0 &&
     * unid==0), this is a false trigger (a swaying branch, a shadow), so scoring
     * the fast frames would just waste iNat calls on nothing (v2.58, operator's
     * frequent-false-trigger fix). The all-background case still files as "no
     * bird" below, unchanged. */
    if (win_k < 0 && job->fast_count > 0 && (ncand > 0 || unid > 0)) {
        s_fast_active = true;               /* live view: FASTBIRD CHECK */
        ESP_LOGI(TAG, "iNat: slow frames saw a bird but declined — scoring %d fast-burst frame(s)",
                 job->fast_count);
        score_range(0, job->fast_count);
        win_k = pick_winner();
        s_fast_active = false;
    }

    bool have = win_k >= 0;
    if (have) {
        *out = cand[win_k].res;
        *win = cand[win_k].roi;
        if (scored_out) *scored_out = cand[win_k].votes;
        if (best_out && best_len) strlcpy(best_out, cand[win_k].best, best_len);
        ESP_LOGI(TAG, "iNat: %s %s %d/%d frame(s) (%u%%)",
                 out->species, cand[win_k].votes >= 2 ? "seconded by" : "solo high-conf",
                 cand[win_k].votes, scored, out->confidence_pct);
    } else if (have_nobird && ncand == 0 && unid == 0) {
        /* Every frame iNat scored had a non-Aves top guess (plant/mammal/…) — file
         * the event as "no bird" (background) rather than unclassified, so
         * vegetation/shadow triggers drop out of the review pile (v2.42). Requires
         * ALL scored frames to agree, so a bird seen in any frame keeps it
         * unclassified. A confident no-bird call, so it isn't escalated to cloud. */
        *out = nobird_res;              /* species "no bird", top3 = the non-bird guesses */
        *win = roi_none();
        if (scored_out) *scored_out = nobird;
        have = true;
        ESP_LOGI(TAG, "iNat: no bird (%d/%d frames non-Aves)", nobird, scored);
    } else {
        ESP_LOGI(TAG, "iNat: no species seconded across %d/%d frame(s) → decline",
                 scored, job->path_count);
    }
    free(cand);
    return have;
}

/* ── Event task: aggregate frames, then write the visit-log row (async) ── */
static void classify_task(void *arg)
{
    cls_job_t job;
    for (;;) {
        if (xQueueReceive(s_jobq, &job, portMAX_DELAY) != pdTRUE) continue;
        s_cls_busy = true;

        classify_result_t best;
        roi_t win = roi_none();
        int   scored = 0;
        const char *source = "";   /* engine that produced the label (§3.2.3) */
        char  pf_detail[320] = ""; /* per-frame iNat scores for the visit log (v2.29) */

        /* Two-tier cascade (§3.2.3, v2.36 — nordic removed):
         *   1. iNaturalist online CV (PRIMARY, free) — when enabled. Scores all
         *      of the event's frames; a species seconded by >=2 frames wins
         *      outright (v2.26). An unsure/uncorroborated result falls through.
         *   2. Claude/Gemini (SECONDARY, paid) — only when still "Unidentified"
         *      or iNat produced nothing, and only if a cloud provider is active.
         *      Last resort because it costs money per call.
         * Each tier degrades to the next on failure, so the row never silently
         * drops work (§3.2). `source` records which engine won. */
        bool have_best = false;
        char best_file[96] = "";   /* frame behind the winning ID (v2.53) */

        if (inat_cv_enabled()) {
            classify_result_t ir;
            roi_t iw = roi_none();
            int   isc = 0;
            char  ibest[96] = "";
            if (inat_event(&job, &ir, &iw, &isc, pf_detail, sizeof(pf_detail),
                           ibest, sizeof(ibest))) {
                best = ir; win = iw; scored = isc;
                strlcpy(best_file, ibest, sizeof(best_file));
                have_best = true; source = "inatcv";
            }
        }

        bool still_unsure = have_best &&
            strcmp(best.species, "Unidentified bird") == 0;
        if ((!have_best || still_unsure) && cloud_enabled()) {
            classify_result_t cres;
            roi_t cwin = roi_none();
            if (cloud_event(&job, &cres, &cwin)) {
                best = cres; win = cwin; scored = 1;
                have_best = true; source = cloud_source_tag(cloud_active());
            }
        }

        /* Motion ROI on EVERY row (v1.99). `win` is the winning frame's crop when
         * a species won, empty on unclassified events. Fall back to job.rois[0],
         * the trigger frame's motion box (always set for a real motion event), so
         * the bird's location is recorded regardless of the outcome. This is the
         * ROI-crop training input + the backfill baseline. */
        roi_t log_roi = roi_is_empty(win) ? job.rois[0] : win;
        char roi_s[24] = "";
        if (!roi_is_empty(log_roi))
            snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                     log_roi.x0, log_roi.y0, log_roi.x1, log_roi.y1);

        /* The MOST RECENT event drives the live badge: true only when it got a
         * real species, so a new unclassified visit clears the last-shown one
         * (v2.29) instead of leaving a stale label over the current bird. */
        s_last_event_ided = have_best && best.latin[0] != '\0';

        char line[720];   /* roomy: + the per-frame column (up to ~10 frames) */
        if (have_best) {
            strlcpy(s_last_species, best.species, sizeof(s_last_species));
            strlcpy(s_last_latin, best.latin, sizeof(s_last_latin));
            s_last_conf = best.confidence_pct;
            if (best.latin[0])
                strlcpy(s_last_file, best_file[0] ? best_file : job.first_path,
                        sizeof(s_last_file));
            ESP_LOGI(TAG, "event @%s: %s (%u%%, top1 '%s', %d/%d frame(s), %ld ms)",
                     job.ts, best.species, best.confidence_pct, best.top_label[0],
                     scored, job.path_count, (long) best.duration_ms);
            char top3[112];
            top3_field(&best, top3, sizeof(top3));
            snprintf(line, sizeof(line), "%s,%s,%u,%d,%s,,%s,%s,%s,%s,%s",
                     job.ts, best.species, best.confidence_pct, job.frames,
                     job.first_path, best.latin, roi_s, top3, source, pf_detail);
        } else {
            snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,,,%s,,%s,%s",
                     job.ts, job.frames, job.first_path, roi_s, source, pf_detail);
        }
        if (storage_append_visit_log(line) != ESP_OK)
            ESP_LOGW(TAG, "visit log append failed");
        s_cls_seq = s_cls_seq + 1;   /* result landed — wakes the "Current" badge (v2.40) */
        /* Clear only when the queue is empty too: back-to-back visits should read
         * as one continuous CLASSIFYING state, not flicker between events. */
        s_cls_busy = (uxQueueMessagesWaiting(s_jobq) > 0);
    }
}

/* ── Re-check one day (§3.4): re-classify logged rows with current model ── */
typedef struct {
    char  ts[20];
    char  path[96];    /* first_frame, web-relative */
    int   frames;
    roi_t roi;         /* roi column as logged (empty when none) */
    bool  add;         /* no existing row — append a new one instead of
                          rewriting (selected photo whose row was wiped by a
                          stats reset, or a follow-up frame) */
} recheck_row_t;

#define RECHECK_MAX_ROWS 256   /* ~32 kB PSRAM; > any plausible day */

static volatile bool s_rc_busy = false;
static volatile int  s_rc_done = 0, s_rc_total = 0;
static char          s_rc_date[11];
static char         *s_rc_filter = NULL;   /* comma-separated first_frame
                                              basenames, NULL = whole day;
                                              owned/freed by the task */

/* Exact token match within the comma-separated list — plain strstr would
 * accept a name that is a substring of another entry. */
static bool rc_in_list(const char *name, const char *list)
{
    size_t nl = strlen(name);
    if (nl == 0) return false;
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += nl) {
        bool at_start = (p == list) || (p[-1] == ',');
        bool at_end   = (p[nl] == '\0' || p[nl] == ',');
        if (at_start && at_end) return true;
    }
    return false;
}

/* stats.c-style field scanner — strtok_r would swallow the always-empty
 * "corrected" column and shift every later field (see stats.c). */
static char *rc_next_field(char **p)
{
    char *start = *p;
    char *comma = strchr(start, ',');
    if (comma) {
        *comma = '\0';
        *p = comma + 1;
    } else {
        char *end = start + strcspn(start, "\r\n");
        *end = '\0';
        *p = end;
    }
    return start;
}

/* Reconstruct an event's saved frames for a best-of-N recheck: the logged
 * first_frame plus the next (frames-1) captures in the day's folder, capped at
 * CLASSIFY_BEST_OF_N. Motion saves an event's frames back-to-back (single
 * motion task, cool-down between events), so the immediate timestamp
 * successors of first_frame ARE that event's follow-up frames — no per-frame
 * paths need storing. This mirrors what the live event task scored, so a
 * recheck matches live quality instead of re-scoring only the first frame,
 * which for a bird is usually the worst (mid-entry / motion-blurred) and was
 * silently downgrading correctly-labelled rows. Returns the frame count (>=1).
 * One directory scan per row — negligible beside the inference it feeds. */
static int rc_event_frames(const recheck_row_t *row,
                           char paths[CLASSIFY_BEST_OF_N][96])
{
    strlcpy(paths[0], row->path, 96);
    int want = row->frames < CLASSIFY_BEST_OF_N ? row->frames : CLASSIFY_BEST_OF_N;
    if (want <= 1) return 1;

    const char *base = strrchr(row->path, '/');
    base = base ? base + 1 : row->path;

    char dir[64];
    snprintf(dir, sizeof(dir), STORAGE_MOUNT_POINT "/captures/%.10s", s_rc_date);
    DIR *d = opendir(dir);
    if (!d) return 1;

    /* keep the (want-1) smallest basenames strictly greater than first_frame,
     * held sorted ascending (want-1 <= CLASSIFY_BEST_OF_N-1, tiny) */
    char succ[CLASSIFY_BEST_OF_N - 1][40];
    int  ns = 0, lim = want - 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        if (!strstr(e->d_name, ".jpg")) continue;
        if (strcmp(e->d_name, base) <= 0) continue;          /* only later frames */
        if (ns == lim && strcmp(e->d_name, succ[ns - 1]) >= 0) continue;
        if (ns < lim) ns++;
        strlcpy(succ[ns - 1], e->d_name, sizeof(succ[0]));   /* place in last slot */
        for (int j = ns - 1; j > 0 && strcmp(succ[j], succ[j - 1]) < 0; j--) {
            char tmp[40];
            strlcpy(tmp,        succ[j],     sizeof(tmp));
            strlcpy(succ[j],    succ[j - 1], sizeof(succ[0]));
            strlcpy(succ[j - 1], tmp,        sizeof(succ[0]));
        }
    }
    closedir(d);

    int nf = 1;
    for (int j = 0; j < ns; j++)
        snprintf(paths[nf++], 96, "/captures/%.10s/%s", s_rc_date, succ[j]);
    return nf;
}

/* Swap one row (matched by timestamp + first_frame — the ms-resolution
 * filename makes that unique) for new_line, filter-to-temp then rename,
 * same pattern as storage_reset_stats_day. One quick rewrite per row keeps
 * the write lock short and commits progress row-by-row, so an interrupted
 * recheck loses nothing already done. */
static bool rc_rewrite_row(const char *csv, const recheck_row_t *row,
                           const char *new_line)
{
    const char *tmp = STORAGE_MOUNT_POINT "/log/recheck.tmp";
    storage_write_lock();
    FILE *in = fopen(csv, "r");
    if (!in) { storage_write_unlock(); return false; }
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); storage_write_unlock(); return false; }
    char line[768];
    bool swapped = false;
    while (fgets(line, sizeof(line), in)) {
        if (!swapped &&
            strncmp(line, row->ts, strlen(row->ts)) == 0 &&
            strstr(line, row->path) != NULL) {
            fputs(new_line, out);
            fputc('\n', out);
            swapped = true;
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (swapped) { unlink(csv); rename(tmp, csv); }
    else          unlink(tmp);
    storage_write_unlock();
    return swapped;
}

static void recheck_task(void *arg)
{
    char csv[64];
    storage_visit_log_path(s_rc_date, csv, sizeof(csv));

    recheck_row_t *rows = (recheck_row_t *) heap_caps_calloc(
        RECHECK_MAX_ROWS, sizeof(recheck_row_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0;
    if (rows) {
        storage_write_lock();          /* stable scan vs concurrent appends */
        FILE *f = fopen(csv, "r");
        if (f) {
            char line[768];
            bool header = true;
            while (n < RECHECK_MAX_ROWS && fgets(line, sizeof(line), f)) {
                if (header) { header = false; continue; }
                if (strncmp(line, s_rc_date, 10) != 0) continue;
                char *p = line;
                char *ts        = rc_next_field(&p);
                rc_next_field(&p);                    /* species */
                rc_next_field(&p);                    /* confidence */
                char *frames    = rc_next_field(&p);
                char *first     = rc_next_field(&p);
                char *corrected = rc_next_field(&p);
                rc_next_field(&p);                    /* latin */
                char *roi_s     = rc_next_field(&p);
                if (!first[0] || corrected[0]) continue;   /* user label wins */
                if (s_rc_filter) {                         /* recheck-selected */
                    const char *base = strrchr(first, '/');
                    if (!rc_in_list(base ? base + 1 : first, s_rc_filter))
                        continue;
                }
                recheck_row_t *r = &rows[n];
                strlcpy(r->ts, ts, sizeof(r->ts));
                strlcpy(r->path, first, sizeof(r->path));
                r->frames = atoi(frames);
                r->roi = roi_none();
                float x0, y0, x1, y1;
                if (sscanf(roi_s, "%f-%f-%f-%f", &x0, &y0, &x1, &y1) == 4) {
                    r->roi.x0 = x0; r->roi.y0 = y0;
                    r->roi.x1 = x1; r->roi.y1 = y1;
                }
                n++;
            }
            fclose(f);
        }
        storage_write_unlock();
    }

    /* Explicitly selected photos with no matching row still deserve one —
     * the row may have been wiped by a stats reset, or the photo is a
     * follow-up frame that never had its own. Synthesize the row from the
     * filename's timestamp ("YYYY-MM-DD_HH-MM-SS-mmm.jpg") and append it
     * after classifying, so recheck-selected doubles as "re-log these
     * photos into statistics". Day-wide rechecks stay row-based — blanket
     * re-logging would turn every follow-up frame into a phantom visit. */
    if (rows && s_rc_filter) {
        const char *list = s_rc_filter;
        while (*list && n < RECHECK_MAX_ROWS) {
            const char *comma = strchr(list, ',');
            size_t tl = comma ? (size_t) (comma - list) : strlen(list);
            char name[64];
            if (tl > 0 && tl < sizeof(name)) {
                memcpy(name, list, tl);
                name[tl] = '\0';
                bool have = false;
                for (int i = 0; i < n && !have; i++) {
                    const char *b = strrchr(rows[i].path, '/');
                    have = strcmp(b ? b + 1 : rows[i].path, name) == 0;
                }
                if (!have && strncmp(name, s_rc_date, 10) == 0 &&
                    strlen(name) >= 20 && name[10] == '_') {
                    recheck_row_t *r = &rows[n];
                    snprintf(r->ts, sizeof(r->ts), "%.10sT%.2s:%.2s:%.2s",
                             name, name + 11, name + 14, name + 17);
                    snprintf(r->path, sizeof(r->path), "/captures/%s/%s",
                             s_rc_date, name);
                    r->frames = 1;
                    r->roi    = roi_none();
                    r->add    = true;
                    n++;
                }
            }
            if (!comma) break;
            list = comma + 1;
        }
    }
    s_rc_total = n;

    /* The per-frame ROI sidecar for the day, loaded ONCE — a day-wide recheck
     * looks up ~1000 frames and re-reading the file each time would be SD-bound. */
    char *frbuf = storage_frameroi_load(s_rc_date);

    for (int i = 0; i < n; i++) {
        recheck_row_t *row = &rows[i];

        /* Reconstruct the event's frames and re-score them via iNaturalist online
         * exactly like a live event (best-of-crop + >=2-frame corroboration +
         * geo-filter), so a recheck matches live quality and applies the current
         * Norway allowlist (v2.36). Every frame's own motion box comes from the
         * sidecar (v2.47), so follow-ups get cropped too. */
        cls_job_t job = {};
        int nf = rc_event_frames(row, job.paths);
        job.path_count = nf;
        /* Give EVERY frame its own motion box from the per-day sidecar (v2.47).
         * Previously only frame 0 got the event row's ROI and follow-ups got
         * roi_none(), so they were scored whole-frame and never cropped — recheck
         * was strictly weaker than live despite claiming to match it. That is what
         * produced the v2.42 false "no bird" verdicts: whole-frame, iNat reads the
         * bread-covered stump as Mammalia and misses a bird at the edge, while the
         * crop finds it (measured: Dompap 11%, Bokfink 10%). */
        for (int k = 0; k < nf; k++) {
            job.rois[k] = (k == 0) ? row->roi : roi_none();
            char rs[24] = "";
            const char *b = strrchr(job.paths[k], '/');
            if (frbuf && storage_frameroi_find(frbuf, b ? b + 1 : job.paths[k], rs, sizeof(rs))) {
                float x0, y0, x1, y1;
                if (sscanf(rs, "%f-%f-%f-%f", &x0, &y0, &x1, &y1) == 4) {
                    job.rois[k].x0 = x0; job.rois[k].y0 = y0;
                    job.rois[k].x1 = x1; job.rois[k].y1 = y1;
                }
            }
        }

        classify_result_t best;
        roi_t win_roi = roi_none();
        char  pf[320] = "";
        int   scored    = 0;
        bool  have_best = inat_cv_enabled() &&
                          inat_event(&job, &best, &win_roi, &scored, pf, sizeof(pf));
        if (have_best) {
            char roi_s[24] = "";
            if (!roi_is_empty(win_roi))   /* always log the motion ROI (see above) */
                snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                         win_roi.x0, win_roi.y0, win_roi.x1, win_roi.y1);
            char top3[112];
            top3_field(&best, top3, sizeof(top3));
            char nl[720];
            /* Recheck re-runs iNaturalist online, so the label is inatcv's. */
            snprintf(nl, sizeof(nl), "%s,%s,%u,%d,%s,,%s,%s,%s,%s,%s",
                     row->ts, best.species, best.confidence_pct, row->frames,
                     row->path, best.latin, roi_s, top3, "inatcv", pf);
            if (row->add) {
                if (storage_append_visit_log(nl) != ESP_OK)
                    ESP_LOGW(TAG, "recheck: append for %s failed", row->path);
            } else {
                rc_rewrite_row(csv, row, nl);
            }
        } else if (!row->add && scored > 0) {
            /* iNat ANSWERED and declined (scored > 0 — at least one frame really
             * got a verdict). A decline with scored == 0 means the calls never got
             * through (TLS dead, rate limit, no network), which is emphatically NOT
             * evidence the label was wrong — clearing it there would let one outage
             * wipe good labels off a whole day, and a long recheck is exactly when
             * the cert-bundle leak (v2.50) kills TLS mid-run. Those rows are left
             * alone (v2.51). Previously the row was left untouched, so a
             * recheck could only ever REPLACE a label with a better one and could
             * never clear a wrong one — a stale "no bird" (or a since-fixed bad ID)
             * survived every re-run, which is exactly what masked the v2.46/v2.47
             * fixes when re-testing (v2.48). Rewrite it back to the unclassified
             * sentinel instead, so declining is a real, visible outcome and the
             * event returns to the review pile. Human labels never reach here —
             * the row scan skips anything with a `corrected` value. */
            char nl[720];
            snprintf(nl, sizeof(nl), "%s,unclassified,0,%d,%s,,,,,,%s",
                     row->ts, row->frames, row->path, pf);
            rc_rewrite_row(csv, row, nl);
        }
        s_rc_done = i + 1;
    }
    free(rows);
    free(s_rc_filter);
    s_rc_filter = NULL;
    free(frbuf);
    ESP_LOGI(TAG, "recheck %s: %d/%d row(s) re-classified", s_rc_date, s_rc_done, s_rc_total);
    s_rc_busy = false;
    vTaskDelete(NULL);
}
#endif /* CONFIG_IDF_TARGET_ESP32S3 */

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t classify_init(void)
{
#if !CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGW(TAG, "species ID unavailable on this target (FSD §3.2) — captures stay 'unclassified'");
    return ESP_OK;
#else
    /* No on-device model any more (v2.36 nordic teardown): classification is
     * iNaturalist online (primary) + optional cloud. The event task/queue always
     * come up so events can be labelled the moment iNat/cloud is enabled — no
     * reboot. When neither is on, classify_submit_event() declines and capture.c
     * writes its 'unclassified' row. 16 deep (~20 KB, the job carries every
     * frame); a busy visit backlogs a few minutes, fine since rows are stamped
     * with the event time. */
    s_jobq = xQueueCreate(16, sizeof(cls_job_t));
    /* 16 KB stack: an iNat/cloud job runs an mbedTLS handshake (~5 KB) plus, on
     * the best-of-crop path, a JPEG decode+re-encode. */
    if (!s_jobq ||
        xTaskCreatePinnedToCore(classify_task, "classify", 16384, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "classify task/queue create failed — species ID disabled");
        return ESP_OK;
    }
    return ESP_OK;
#endif
}

/* "Available" now means an online classifier can run: iNaturalist (primary) or a
 * cloud provider. There is no on-device model any more (v2.36). */
bool classify_available(void) { return inat_cv_enabled() || cloud_enabled(); }

bool classify_submit_event(const char (*paths)[96], const roi_t *rois,
                           int path_count, int fast_count, const char *ts,
                           int frames, const char *first_path)
{
#if CONFIG_IDF_TARGET_ESP32S3
    /* iNat (primary) or cloud must be on; otherwise decline so capture.c writes
     * the "unclassified" row (§3.2.3). */
    if ((!inat_cv_enabled() && !cloud_enabled()) ||
        path_count <= 0 || !s_jobq) return false;
    cls_job_t job = {};
    job.frames = frames;
    strlcpy(job.ts, ts, sizeof(job.ts));
    strlcpy(job.first_path, first_path, sizeof(job.first_path));
    int n = path_count < CLASSIFY_BEST_OF_N ? path_count : CLASSIFY_BEST_OF_N;
    for (int i = 0; i < n; i++) {
        strlcpy(job.paths[i], paths[i], sizeof(job.paths[0]));
        job.rois[i] = rois ? rois[i] : roi_none();
    }
    job.path_count = n;
    /* The fast-burst frames are the first fast_count of paths[]; cap to what
     * actually fit (n) so a truncated event can't over-count them (v2.56). */
    job.fast_count = fast_count < 0 ? 0 : (fast_count > n ? n : fast_count);
    /* A full queue means every buffered slot is a real visit waiting for a
     * label — giving up here writes a permanent "unclassified" row for a
     * moment of transient pressure. Wait up to 15 s for a slot instead — the
     * 16-deep queue is the real buffer (a job now scores every frame and can
     * run tens of seconds, so this wait no longer spans a whole job, only
     * bridges brief contention). This runs in the motion task after the event's
     * frames are safely on SD; the cost of blocking is a longer cooldown, not
     * lost images. Bounded so a wedged classifier can't stall capture forever. */
    if (xQueueSend(s_jobq, &job, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "classify queue full for 15 s — event logged unclassified");
        return false;
    }
    return true;
#else
    (void) paths; (void) rois; (void) path_count; (void) ts; (void) frames; (void) first_path;
    return false;
#endif
}

bool classify_recheck_start(const char *date, const char *files)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!inat_cv_enabled() || !storage_sd_present()) return false;
    if (!date || strlen(date) != 10) return false;
    if (s_rc_busy) return false;
    s_rc_busy  = true;
    s_rc_done  = 0;
    s_rc_total = 0;
    strlcpy(s_rc_date, date, sizeof(s_rc_date));
    s_rc_filter = NULL;
    if (files && files[0]) {           /* copied — the caller's body is transient */
        s_rc_filter = (char *) heap_caps_malloc(strlen(files) + 1,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rc_filter) { s_rc_busy = false; return false; }
        strcpy(s_rc_filter, files);
    }
    /* Priority 2 — below the event task (3), so live visits always win the
     * run mutex first and a recheck only fills the gaps. */
    if (xTaskCreatePinnedToCore(recheck_task, "recheck", 12288, NULL, 2, NULL, 1)
        != pdPASS) {
        free(s_rc_filter);
        s_rc_filter = NULL;
        s_rc_busy = false;
        return false;
    }
    return true;
#else
    (void) date; (void) files;
    return false;
#endif
}

void classify_recheck_status(bool *busy, int *done, int *total,
                             char *date, size_t date_len)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (busy)  *busy  = s_rc_busy;
    if (done)  *done  = s_rc_done;
    if (total) *total = s_rc_total;
    if (date && date_len) strlcpy(date, s_rc_date, date_len);
#else
    if (busy)  *busy  = false;
    if (done)  *done  = 0;
    if (total) *total = 0;
    if (date && date_len) date[0] = '\0';
#endif
}

/* Manual one-shot (POST /api/classify[-file]): now an iNaturalist round-trip on
 * the posted/read JPEG, since there's no on-device model to run (v2.36). */
esp_err_t classify_run_sync(const uint8_t *jpeg, size_t len, classify_result_t *out)
{
    return inat_classify_jpeg(jpeg, len, out);
}

int32_t     classify_last_duration_ms(void) { return s_last_ms; }
const char *classify_model_name(void)       { return inat_cv_enabled() ? "iNaturalist online" : ""; }
int         classify_label_count(void)      { return (int) TARGET_SPECIES_N; }
int         classify_region_matches(void)   { return (int) TARGET_SPECIES_N; }
const char *classify_last_species(void)     { return s_last_species; }
const char *classify_last_latin(void)       { return s_last_latin; }
const char *classify_last_file(void)        { return s_last_file; }
uint8_t     classify_last_confidence(void)  { return s_last_conf; }
bool        classify_last_event_identified(void) { return s_last_event_ided; }
uint32_t    classify_result_seq(void)       { return s_cls_seq; }
bool        classify_busy(void)             { return s_cls_busy; }
bool        classify_fastfallback_active(void) { return s_fast_active; }

/* The relabel picker's vocabulary is now the curated Norway target-species list
 * (target_species.h) — there's no model label file any more (v2.36). Returns
 * "Latin (Common)", the same shape the picker parsed from model labels. */
const char *classify_label(int i)
{
    static char buf[128];
    if (i < 0 || i >= (int) TARGET_SPECIES_N) return "";
    snprintf(buf, sizeof(buf), "%s (%s)", TARGET_SPECIES[i].latin, TARGET_SPECIES[i].common);
    return buf;
}

/* Every target species is in-region (the list IS the Norway set), so the picker
 * offers them all (§3.4). */
bool classify_label_region(int i)
{
    return i >= 0 && i < (int) TARGET_SPECIES_N;
}
