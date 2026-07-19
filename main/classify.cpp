/* On-device species identification (FSD §3.2).
 *
 * Model: a quantized (int8) MobileNetV2-class bird classifier in TFLite
 * format, loaded from /sd/model at boot — users swap regions/models by
 * replacing the file (or POST /model/upload), no reflash. The v1 default
 * documented in docs/MODEL.md is Google's iNaturalist-birds model
 * (965 classes incl. a "background" guard class, Apache-2.0).
 *
 * Pipeline: motion.c's event keeps a PSRAM copy of the best (first) frame;
 * capture_event_finish() queues it here and returns immediately — capture
 * is never delayed (§3.2). This task decodes the JPEG (esp_jpeg, scaled),
 * center-crops + nearest-resizes to the model's 224x224 input, invokes
 * TFLM (esp-nn kernels on the S3's vector unit), applies the confidence
 * threshold and background-class guard, and only then writes the event's
 * visit-log row with the real species + confidence.
 *
 * Everything heavyweight (model ~3.5 MB, tensor arena 3 MB, decode buffer)
 * lives in PSRAM; internal RAM is untouched beyond the task stack. */
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#endif

static const char *TAG = "classify";

static bool             s_available = false;
static char             s_model_name[48] = "";
static int              s_label_count = 0;
static int              s_region_matches = 0; /* labels whose binomial is in the
                                                 Northern-European set (§3.2.1) */
static char           **s_labels = NULL;      /* pointers into s_label_buf */
static char            *s_label_buf = NULL;
static float           *s_scores = NULL;       /* dequantized output scratch, sized
                                                  s_label_count (run mutex serializes) */
static float           *s_scores_flip = NULL;  /* TTA flip-pass scratch (§3.2/v1.55) */
static int              s_out_classes = 0;     /* classes the model actually outputs */
static volatile int32_t s_last_ms = -1;
static char             s_last_species[64] = "";
static char             s_last_latin[64] = "";
static uint8_t          s_last_conf = 0;      /* confidence % of the last event */

#if CONFIG_IDF_TARGET_ESP32S3

#define CLS_INPUT_SIDE   224
#define CLS_ARENA_BYTES  (3 * 1024 * 1024)
#define CLS_DECODE_MAX   (1536 * 1024)   /* cap on the RGB888 decode buffer */

static uint8_t                  *s_model_buf = NULL;
static uint8_t                  *s_arena = NULL;
static tflite::MicroInterpreter *s_interp = NULL;
static SemaphoreHandle_t         s_run_mtx = NULL;

typedef struct {
    char     ts[24];
    int      frames;
    char     first_path[96];
    char     paths[CLASSIFY_BEST_OF_N][96];  /* saved-frame paths to score */
    roi_t    rois[CLASSIFY_BEST_OF_N];       /* per-frame motion zoom (§3.1) */
    int      path_count;
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

static bool is_model_filename(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && strcasecmp(dot, ".tflite") == 0;
}

/* g_settings.region names the model file (§3.2 region selector); empty =
 * auto: first .tflite found in /sd/model. */
static bool pick_model_file(char *name, size_t name_len)
{
    if (g_settings.region[0] && is_model_filename(g_settings.region)) {
        char p[96];
        snprintf(p, sizeof(p), STORAGE_MOUNT_POINT "/model/%.48s", g_settings.region);
        struct stat st;
        if (stat(p, &st) == 0) {
            strlcpy(name, g_settings.region, name_len);
            return true;
        }
        ESP_LOGW(TAG, "configured region model '%s' not on SD — falling back to auto",
                 g_settings.region);
    }
    DIR *d = opendir(STORAGE_MOUNT_POINT "/model");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG || !is_model_filename(e->d_name)) continue;
        strlcpy(name, e->d_name, name_len);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

/* Region filter (§3.2.1): a label is allowed when it carries no scientific
 * binomial (e.g. the "background" guard class — never hidden) or when its
 * binomial is in the Northern-European set. Callers apply this only when
 * g_settings.region_filter is on AND the loaded model actually matches the
 * set (s_region_matches > 0), so an unrelated model is never over-filtered. */
static bool label_in_region(const char *label)
{
    const char *open = strrchr(label, '(');
    if (!open) return true;               /* no binomial to place → keep */
    size_t llen = (size_t) (open - label);
    while (llen > 0 && label[llen - 1] == ' ') llen--;
    char latin[64];
    if (llen >= sizeof(latin)) llen = sizeof(latin) - 1;
    memcpy(latin, label, llen);
    latin[llen] = '\0';
    return species_in_region(latin);
}

/* Labels file: "<model>.txt" beside the model (one label per line, index-
 * aligned with the model's output tensor), else labels.txt. */
static bool load_labels(const char *model_name)
{
    char p[96];
    snprintf(p, sizeof(p), STORAGE_MOUNT_POINT "/model/%.48s", model_name);
    char *dot = strrchr(p, '.');
    if (dot) strcpy(dot, ".txt");
    size_t len = 0;
    s_label_buf = (char *) load_file_psram(p, &len);
    if (!s_label_buf) {
        snprintf(p, sizeof(p), STORAGE_MOUNT_POINT "/model/labels.txt");
        s_label_buf = (char *) load_file_psram(p, &len);
    }
    if (!s_label_buf) return false;

    int lines = 0;
    for (char *c = s_label_buf; *c; c++)
        if (*c == '\n') lines++;
    lines++;   /* possible last line without newline */
    s_labels = (char **) heap_caps_malloc(lines * sizeof(char *),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_labels) return false;

    s_label_count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(s_label_buf, "\n", &save); tok && s_label_count < lines;
         tok = strtok_r(NULL, "\n", &save)) {
        char *cr = strchr(tok, '\r');
        if (cr) *cr = '\0';
        if (tok[0]) s_labels[s_label_count++] = tok;
    }

    /* Count how many labels carry an in-region binomial, so the region filter
     * can auto-disable itself for a model that isn't the expected set (§3.2.1)
     * rather than silently classifying every frame as the guard class. */
    s_region_matches = 0;
    for (int i = 0; i < s_label_count; i++)
        if (strrchr(s_labels[i], '(') && label_in_region(s_labels[i]))
            s_region_matches++;
    ESP_LOGI(TAG, "%d/%d labels are in the Northern-European set", s_region_matches, s_label_count);

    return s_label_count > 0;
}

/* ── JPEG → 224x224 RGB888 model input ──────────────────────────────────── */
/* rot: mount-correction rotation (§5). 0/180 are already baked into the JPEG
 * bytes by the sensor (camera_set_rotation, hmirror+vflip) — the OV2640 has
 * no 90/270 hardware path, so those two angles are corrected here instead by
 * permuting the crop/resize indices; the sampling itself (nearest-neighbor,
 * same source pixels) is unchanged, so rotation costs no extra quality. */
static esp_err_t decode_to_input(const uint8_t *jpeg, size_t len, uint8_t *dst224,
                                 rotation_t rot, roi_t roi, bool hflip)
{
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata      = (uint8_t *) jpeg;
    cfg.indata_size = len;
    cfg.out_format  = JPEG_IMAGE_FORMAT_RGB888;

    esp_jpeg_image_output_t info = {};
    esp_err_t err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "jpeg header parse failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* Halve while the *crop region* (the ROI when zooming, else the frame)
     * stays >= the input side, then further while the buffer is over budget.
     * ROI-aware: a small ROI keeps the decode at higher resolution so the
     * cropped bird lands at (near-)native pixels instead of being decoded
     * small and upscaled — SVGA with a half-frame ROI now decodes 800x600
     * (1.44 MB, within budget) instead of 400x300. */
    float rfw = 1.0f, rfh = 1.0f;
    if (!roi_is_empty(roi)) { rfw = roi.x1 - roi.x0; rfh = roi.y1 - roi.y0; }
    int scale = 0, w = info.width, h = info.height;
    while (scale < 3 &&
           (float) (w >> 1) * rfw >= CLS_INPUT_SIDE &&
           (float) (h >> 1) * rfh >= CLS_INPUT_SIDE) {
        w >>= 1; h >>= 1; scale++;
    }
    while (scale < 3 && (size_t) w * h * 3 > CLS_DECODE_MAX) {
        w >>= 1; h >>= 1; scale++;
    }
    if ((size_t) w * h * 3 > CLS_DECODE_MAX) {
        ESP_LOGW(TAG, "image too large to decode (%ux%u)", info.width, info.height);
        return ESP_ERR_NO_MEM;
    }

    /* The ROI-aware sizing above asks for what the zoom *wants*; free PSRAM
     * decides what it *gets*. A tight ROI at SXGA wants 640x512 (983 KB),
     * which routinely exceeds the largest free block once the model + arena
     * are resident — before v1.45 that failed silently and the whole event
     * was logged "unclassified". Degrade the decode scale until the buffer
     * fits instead: a bird cropped from a smaller decode still classifies. */
    size_t   out_size;
    uint8_t *rgb;
    for (;;) {
        out_size = (size_t) w * h * 3 + 16;
        rgb = (uint8_t *) heap_caps_malloc(out_size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (rgb || scale >= 3) break;
        w >>= 1; h >>= 1; scale++;
        ESP_LOGW(TAG, "decode buffer %u KB unavailable — degrading to %ux%u",
                 (unsigned) (out_size / 1024), w, h);
    }
    if (!rgb) {
        ESP_LOGW(TAG, "decode buffer alloc failed (%u KB even at 1/8 scale)",
                 (unsigned) (out_size / 1024));
        return ESP_ERR_NO_MEM;
    }

    cfg.outbuf      = rgb;
    cfg.outbuf_size = out_size;
    cfg.out_scale   = (esp_jpeg_image_scale_t) scale;
    esp_jpeg_image_output_t out = {};
    err = esp_jpeg_decode(&cfg, &out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        free(rgb);
        return ESP_FAIL;
    }
    w = out.width;
    h = out.height;

    /* Crop region: the motion ROI (zoomed species ID, §3.1) when non-empty,
     * else the whole frame. The ROI is in source-frame axes (same coordinates
     * motion.c differenced), so it's applied here before the rotation permute
     * below — orientation-independent. Then square-crop within it and
     * nearest-neighbor resize to 224x224. */
    int rx0 = 0, ry0 = 0, rw = w, rh = h;
    if (!roi_is_empty(roi)) {
        rx0 = (int) (roi.x0 * w + 0.5f);
        ry0 = (int) (roi.y0 * h + 0.5f);
        rw  = (int) ((roi.x1 - roi.x0) * w + 0.5f);
        rh  = (int) ((roi.y1 - roi.y0) * h + 0.5f);
        if (rx0 < 0) rx0 = 0;
        if (ry0 < 0) ry0 = 0;
        if (rw < 1)  rw = 1;
        if (rh < 1)  rh = 1;
        if (rx0 + rw > w) rw = w - rx0;
        if (ry0 + rh > h) rh = h - ry0;
    }
    /* Square-EXPAND the crop: grow the short side to match the long one
     * (clamped to the frame) so an elongated ROI is fully kept with extra
     * context, instead of truncating its ends. For the whole-frame case this
     * clamps back to min(w,h) — the classic center-crop, unchanged. */
    int side = rw > rh ? rw : rh;
    int frame_min = w < h ? w : h;
    if (side > frame_min) side = frame_min;
    int x0 = rx0 + rw / 2 - side / 2, y0 = ry0 + rh / 2 - side / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + side > w) x0 = w - side;
    if (y0 + side > h) y0 = h - side;
    bool sw_rotate = (rot == ROTATE_90 || rot == ROTATE_270);
    for (int y = 0; y < CLS_INPUT_SIDE; y++) {
        int py = y * side / CLS_INPUT_SIDE;
        uint8_t *drow = dst224 + (size_t) y * CLS_INPUT_SIDE * 3;
        if (!sw_rotate) {
            int sy = y0 + py;
            const uint8_t *row = rgb + ((size_t) sy * w + x0) * 3;
            for (int x = 0; x < CLS_INPUT_SIDE; x++) {
                int sx = x * side / CLS_INPUT_SIDE;
                int dx = hflip ? CLS_INPUT_SIDE - 1 - x : x;
                drow[dx * 3 + 0] = row[sx * 3 + 0];
                drow[dx * 3 + 1] = row[sx * 3 + 1];
                drow[dx * 3 + 2] = row[sx * 3 + 2];
            }
        } else {
            for (int x = 0; x < CLS_INPUT_SIDE; x++) {
                int px = x * side / CLS_INPUT_SIDE;
                /* rotate the (px,py) crop-local coordinate 90/270 within the
                 * side x side square before mapping back to source pixels */
                int lx, ly;
                if (rot == ROTATE_90) { lx = py;              ly = side - 1 - px; }
                else                  { lx = side - 1 - py;   ly = px; }
                const uint8_t *sp = rgb + ((size_t) (y0 + ly) * w + (x0 + lx)) * 3;
                int dx = hflip ? CLS_INPUT_SIDE - 1 - x : x;
                drow[dx * 3 + 0] = sp[0];
                drow[dx * 3 + 1] = sp[1];
                drow[dx * 3 + 2] = sp[2];
            }
        }
    }
    free(rgb);
    return ESP_OK;
}

/* ── Decision logic (§3.2): threshold + background guard ────────────────── */
static void make_decision(classify_result_t *r)
{
    /* Common name is the text in parens ("Erithacus rubecula (European
     * Robin)" -> "European Robin"); labels without parens (e.g.
     * "background") pass through with no latin binomial. */
    char name[64];
    char latin[64] = "";
    const char *open = strrchr(r->top_label[0], '(');
    if (open && strchr(open, ')')) {
        strlcpy(name, open + 1, sizeof(name));
        char *close = strrchr(name, ')');
        if (close) *close = '\0';
        size_t llen = (size_t) (open - r->top_label[0]);
        if (llen >= sizeof(latin)) llen = sizeof(latin) - 1;
        memcpy(latin, r->top_label[0], llen);
        latin[llen] = '\0';
        while (llen > 0 && latin[llen - 1] == ' ') latin[--llen] = '\0';
    } else {
        strlcpy(name, r->top_label[0], sizeof(name));
    }
    for (char *c = name; *c; c++)          /* CSV-safe */
        if (*c == ',') *c = ';';

    r->confidence_pct = r->top_pct[0];
    r->latin[0] = '\0';
    if (strcasecmp(r->top_label[0], "background") == 0)
        strlcpy(r->species,
                r->top_pct[0] >= g_settings.confidence_pct ? "no bird"
                                                           : "Unidentified bird",
                sizeof(r->species));
    else if (r->top_pct[0] >= g_settings.confidence_pct) {
        strlcpy(r->species, name, sizeof(r->species));
        strlcpy(r->latin, latin, sizeof(r->latin));
    } else
        strlcpy(r->species, "Unidentified bird", sizeof(r->species));
}

/* Rank a dequantized score vector (region-filtered), fill top-3 + confidence,
 * and decide. Shared by single-frame inference and multi-frame evidence pooling
 * (Phase 0, §3.2/v1.54) so a pooled distribution is judged with exactly the
 * same threshold and background guard as one frame. Region filter (§3.2.1):
 * confidence is NOT renormalized — an out-of-region winner simply loses its
 * class and the best in-region score falls below threshold; skipped when the
 * loaded model doesn't match the set so it can't blank everything. */
static void decide_from_scores(const float *scores, int n, classify_result_t *out)
{
    bool rf = g_settings.region_filter && s_region_matches > 0;
    int best[3] = { -1, -1, -1 };
    for (int i = 0; i < n; i++) {
        if (rf && !label_in_region(s_labels[i])) continue;
        for (int k = 0; k < 3; k++) {
            if (best[k] < 0 || scores[i] > scores[best[k]]) {
                for (int m = 2; m > k; m--) best[m] = best[m - 1];
                best[k] = i;
                break;
            }
        }
    }
    for (int k = 0; k < 3; k++) { out->top_label[k][0] = '\0'; out->top_pct[k] = 0; }
    for (int k = 0; k < 3; k++) {
        if (best[k] < 0) break;
        strlcpy(out->top_label[k], s_labels[best[k]], sizeof(out->top_label[k]));
        float p = scores[best[k]] < 0 ? 0 : scores[best[k]];
        out->top_pct[k] = (uint8_t) (p * 100.0f + 0.5f);
    }
    make_decision(out);
}

/* One decode+invoke pass. Decodes the JPEG (optionally horizontally mirrored)
 * into the model input, runs inference, and writes the dequantized score vector
 * into dst (>= s_label_count floats). *n_out gets the class count, *dur_ms the
 * inference time. Shared by run_locked's normal and TTA-flip passes. */
static esp_err_t infer_scores(const uint8_t *jpeg, size_t len, roi_t roi,
                              bool hflip, float *dst, int *n_out, int32_t *dur_ms)
{
    TfLiteTensor *in = s_interp->input(0);
    esp_err_t err = decode_to_input(jpeg, len, in->data.uint8,
                                    g_settings.rotation, roi, hflip);
    if (err != ESP_OK) return err;
    /* Decoder writes uint8 pixels; the int8 model expects pixel - 128
     * (same scale, zero point shifted). XOR 0x80 is that, in place. */
    for (size_t i = 0; i < (size_t) CLS_INPUT_SIDE * CLS_INPUT_SIDE * 3; i++)
        in->data.uint8[i] ^= 0x80;

    int64_t t0 = esp_timer_get_time();
    if (s_interp->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return ESP_FAIL;
    }
    *dur_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);

    TfLiteTensor *ot = s_interp->output(0);
    const int8_t *sc8 = ot->data.int8;
    int n = ot->dims->data[1];
    if (n > s_label_count) n = s_label_count;
    *n_out = n;
    float zp = ot->params.zero_point, scale = ot->params.scale;
    for (int i = 0; i < n; i++) dst[i] = ((float) sc8[i] - zp) * scale;
    return ESP_OK;
}

/* ── Inference core (shared by the event task and /api/classify) ────────── */
/* raw_out (nullable): receives the full dequantized score vector (s_out_classes
 * entries) for evidence pooling; NULL when only the decision is needed. */
static esp_err_t run_locked(const uint8_t *jpeg, size_t len, classify_result_t *out,
                            roi_t roi, float *raw_out)
{
    memset(out, 0, sizeof(*out));

    /* Zoom is opt-out (§3.2): an empty roi already means whole-frame, and the
     * setting lets the user disable ROI cropping entirely. */
    if (!g_settings.detect_zoom) roi = roi_none();

    if (!s_scores) {
        s_scores = (float *) heap_caps_malloc(s_label_count * sizeof(float),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scores) return ESP_ERR_NO_MEM;
    }
    int n = 0;
    int32_t dur = 0;
    esp_err_t err = infer_scores(jpeg, len, roi, false, s_scores, &n, &dur);
    if (err != ESP_OK) return err;
    out->duration_ms = dur;

    /* Test-time augmentation (§3.2/v1.55): average the scores of the frame and
     * its horizontal mirror. A left/right flip is label-preserving for birds and
     * gives the pose-sensitive model a second look, lifting confidence on hard
     * poses at ~2x inference time. Degrades to the single pass on OOM. */
    if (g_settings.tta) {
        if (!s_scores_flip)
            s_scores_flip = (float *) heap_caps_malloc(s_label_count * sizeof(float),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_scores_flip) {
            int nf = 0;
            int32_t durf = 0;
            if (infer_scores(jpeg, len, roi, true, s_scores_flip, &nf, &durf) == ESP_OK) {
                int m = nf < n ? nf : n;
                for (int i = 0; i < m; i++)
                    s_scores[i] = 0.5f * (s_scores[i] + s_scores_flip[i]);
                out->duration_ms += durf;
            }
        }
    }
    s_last_ms = out->duration_ms;

    s_out_classes = n;
    if (raw_out) memcpy(raw_out, s_scores, (size_t) n * sizeof(float));

    decide_from_scores(s_scores, n, out);
    return ESP_OK;
}

/* One inference, taking the run mutex for just this frame (event loops lock
 * per frame so a manual /api/classify isn't blocked for the whole run). */
static esp_err_t run_one(const uint8_t *jpeg, size_t len, classify_result_t *out,
                         roi_t roi, float *raw_out)
{
    xSemaphoreTake(s_run_mtx, portMAX_DELAY);
    esp_err_t err = run_locked(jpeg, len, out, roi, raw_out);
    xSemaphoreGive(s_run_mtx);
    return err;
}

/* best-of-N ranking: a decided real species (non-empty latin — the "no bird"/
 * "Unidentified bird"/"unclassified" sentinels leave it empty) always beats a
 * sentinel, so a confident background frame can't outrank a real ID from
 * another frame; among equals, higher confidence wins. */
static bool result_better(const classify_result_t *n, const classify_result_t *c)
{
    bool nr = n->latin[0] != '\0', cr = c->latin[0] != '\0';
    if (nr != cr) return nr;
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

/* ── Multi-frame aggregation (§3.2, Phase 0/v1.54) ───────────────────────── */
/* Aggregate an event's frames into one decision. Two aggregators are computed
 * and the more confident real species wins (result_better):
 *   • best-of-N  — the single most-confident frame (the pre-v1.54 behaviour),
 *                  which a lone good frame among background ones needs so it
 *                  isn't diluted; and
 *   • evidence pooling — the mean softmax across every scored frame, which
 *                  combines weak views (a head-on frame + a side-on frame) into
 *                  a confident call the underconfident model won't reach alone.
 * Keeping best-of-N as a floor means this never scores below the old logic.
 * Falls back to one whole-frame pass if the winner is a sentinel and any frame
 * was zoom-cropped (a miss-cropped bird is often still identifiable whole).
 * `paths` are web-relative ("/captures/.."); fills *best_out and *win_roi (the
 * ROI to log — empty when a pooled or whole-frame result won, as those aren't
 * tied to one frame's crop). Returns false if no frame could be scored. */
static bool aggregate_frames(const char (*paths)[96], const roi_t *rois, int nf,
                             int *scored_out, classify_result_t *best_out, roi_t *win_roi)
{
    float *accum = (float *) heap_caps_calloc(s_label_count, sizeof(float),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *tmp   = (float *) heap_caps_malloc(s_label_count * sizeof(float),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool pool = accum && tmp;                 /* pooling degrades to best-of-N on OOM */

    classify_result_t best;
    bool  have_best = false;
    int   scored = 0, best_idx = 0;
    bool  zoomed_any = false;
    roi_t win = roi_none();

    for (int i = 0; i < nf; i++) {
        char full[128];
        snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", paths[i]);
        size_t len = 0;
        uint8_t *buf = load_file_psram(full, &len);
        if (!buf) { ESP_LOGW(TAG, "aggregate: read %s failed", full); continue; }
        /* "zoom was attempted", not "succeeded" — the rescue must still fire
         * when every zoomed run failed outright. */
        if (g_settings.detect_zoom && !roi_is_empty(rois[i])) zoomed_any = true;
        classify_result_t r;
        esp_err_t err = run_one(buf, len, &r, rois[i], pool ? tmp : NULL);
        free(buf);
        if (err != ESP_OK) continue;
        scored++;
        if (pool) for (int k = 0; k < s_out_classes; k++) accum[k] += tmp[k];
        if (!have_best || result_better(&r, &best)) { best = r; best_idx = i; win = rois[i]; have_best = true; }
    }

    /* Pooled decision — only meaningful with >=2 frames of evidence. */
    if (pool && scored >= 2) {
        for (int k = 0; k < s_out_classes; k++) accum[k] /= scored;
        classify_result_t pooled;
        memset(&pooled, 0, sizeof(pooled));
        decide_from_scores(accum, s_out_classes, &pooled);
        if (!have_best || result_better(&pooled, &best)) {
            ESP_LOGI(TAG, "pooled(%d frames) won: %s (%u%%)",
                     scored, pooled.species, pooled.confidence_pct);
            best = pooled;
            win  = roi_none();               /* multi-frame: no single crop */
        }
    }
    free(accum);
    free(tmp);

    if ((!have_best || best.latin[0] == '\0') && zoomed_any) {
        char full[128];
        snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", paths[best_idx]);
        size_t len = 0;
        uint8_t *buf = load_file_psram(full, &len);
        if (buf) {
            classify_result_t w;
            if (run_one(buf, len, &w, roi_none(), NULL) == ESP_OK &&
                (!have_best || result_better(&w, &best))) {
                ESP_LOGI(TAG, "whole-frame fallback rescued: %s (%u%%)",
                         w.species, w.confidence_pct);
                best = w;
                have_best = true;
                win = roi_none();
            }
            free(buf);
        }
    }

    if (scored_out) *scored_out = scored;
    if (have_best) { *best_out = best; *win_roi = win; }
    return have_best;
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
static bool inat_event(const cls_job_t *job, classify_result_t *out, roi_t *win)
{
    if (!inat_cv_enabled()) return false;

    char full[128];
    snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job->paths[0]);
    if (inat_classify_file(full, out) != ESP_OK) {
        ESP_LOGW(TAG, "iNat CV failed (%s) — falling back to the on-device model",
                 inat_last_error());
        return false;
    }
    if (out->latin[0] == '\0') return false;   /* unsure → let nordic try */
    *win = job->rois[0];
    return true;
}

/* ── Event task: aggregate frames, then write the visit-log row (async) ── */
static void classify_task(void *arg)
{
    cls_job_t job;
    for (;;) {
        if (xQueueReceive(s_jobq, &job, portMAX_DELAY) != pdTRUE) continue;

        classify_result_t best;
        roi_t win = roi_none();
        int   scored = 0;
        const char *source = "";   /* engine that produced the label (§3.2.3) */

        /* Three-tier cascade (§3.2.3), cheapest-good-answer first:
         *   1. iNaturalist online CV (PRIMARY, free) — when enabled. A confident
         *      bird species wins outright; an unsure result falls through.
         *   2. on-device nordic model (SECONDARY / offline fallback) — runs when
         *      iNat didn't confidently ID (disabled, unsure, or failed). Owns the
         *      "no bird" call. A confident species or "no bird" is kept as-is.
         *   3. Claude/Gemini (TERTIARY, paid) — only when still "Unidentified",
         *      and only if a cloud provider is active. Last resort because it
         *      costs money per call.
         * Each tier degrades to the next on failure, so the row never silently
         * drops work (§3.2). `source` records which engine won. (The optional
         * periodic on-device iNat batch is a separate out-of-band path.) */
        bool have_best = false;

        if (inat_cv_enabled()) {
            classify_result_t ir;
            roi_t iw = roi_none();
            if (inat_event(&job, &ir, &iw)) {
                best = ir; win = iw; scored = 1;
                have_best = true; source = "inatcv";
            }
        }

        if (!have_best && s_available && g_settings.ondevice_enabled) {
            have_best = aggregate_frames(job.paths, job.rois, job.path_count,
                                         &scored, &best, &win);
            if (have_best) source = "nordic";
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

        /* Motion ROI on EVERY row (v1.99). `win` is the winning frame's crop
         * when best-of-N won, but empty when the multi-frame POOLED result won
         * (aggregate_frames) and on unclassified events — precisely the rows
         * that were left ROI-less, including the rescue birds (model-missed
         * species land unclassified, then a human relabel preserves whatever
         * roi is here). Fall back to job.rois[0], the trigger frame's motion box
         * (always set for a real motion event), so the bird's location is
         * recorded regardless of which aggregator won or whether the model
         * identified it. This is ROI-crop training input + the backfill baseline;
         * detect_zoom only governs whether INFERENCE crops to it. */
        roi_t log_roi = roi_is_empty(win) ? job.rois[0] : win;
        char roi_s[24] = "";
        if (!roi_is_empty(log_roi))
            snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                     log_roi.x0, log_roi.y0, log_roi.x1, log_roi.y1);

        char line[400];
        if (have_best) {
            strlcpy(s_last_species, best.species, sizeof(s_last_species));
            strlcpy(s_last_latin, best.latin, sizeof(s_last_latin));
            s_last_conf = best.confidence_pct;
            ESP_LOGI(TAG, "event @%s: %s (%u%%, top1 '%s', %d/%d frame(s), %ld ms)",
                     job.ts, best.species, best.confidence_pct, best.top_label[0],
                     scored, job.path_count, (long) best.duration_ms);
            char top3[112];
            top3_field(&best, top3, sizeof(top3));
            snprintf(line, sizeof(line), "%s,%s,%u,%d,%s,,%s,%s,%s,%s",
                     job.ts, best.species, best.confidence_pct, job.frames,
                     job.first_path, best.latin, roi_s, top3, source);
        } else {
            snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,,,%s,,%s",
                     job.ts, job.frames, job.first_path, roi_s, source);
        }
        if (storage_append_visit_log(line) != ESP_OK)
            ESP_LOGW(TAG, "visit log append failed");
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
    char line[400];
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
            char line[400];
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

    for (int i = 0; i < n; i++) {
        recheck_row_t *row = &rows[i];

        /* Reconstruct the event's frames and aggregate them exactly like a live
         * event (best-of-N + evidence pooling + rescue) so a recheck matches
         * live quality (v1.49/v1.54). Only frame 0's ROI is known; follow-ups
         * score whole-frame (and run_locked forces whole-frame when zoom off). */
        char  paths[CLASSIFY_BEST_OF_N][96];
        int   nf = rc_event_frames(row, paths);
        roi_t rois[CLASSIFY_BEST_OF_N];
        rois[0] = row->roi;
        for (int k = 1; k < nf; k++) rois[k] = roi_none();

        classify_result_t best;
        roi_t win_roi = roi_none();
        int   scored = 0;
        bool  have_best = aggregate_frames(paths, rois, nf, &scored, &best, &win_roi);
        if (have_best) {
            char roi_s[24] = "";
            if (!roi_is_empty(win_roi))   /* always log the motion ROI (see above) */
                snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                         win_roi.x0, win_roi.y0, win_roi.x1, win_roi.y1);
            char top3[112];
            top3_field(&best, top3, sizeof(top3));
            char nl[400];
            /* Recheck re-runs the on-device model, so the label is nordic's. */
            snprintf(nl, sizeof(nl), "%s,%s,%u,%d,%s,,%s,%s,%s,%s",
                     row->ts, best.species, best.confidence_pct, row->frames,
                     row->path, best.latin, roi_s, top3, "nordic");
            if (row->add) {
                if (storage_append_visit_log(nl) != ESP_OK)
                    ESP_LOGW(TAG, "recheck: append for %s failed", row->path);
            } else {
                rc_rewrite_row(csv, row, nl);
            }
        }
        s_rc_done = i + 1;
    }
    free(rows);
    free(s_rc_filter);
    s_rc_filter = NULL;
    ESP_LOGI(TAG, "recheck %s: %d/%d row(s) re-classified", s_rc_date, s_rc_done, s_rc_total);
    s_rc_busy = false;
    vTaskDelete(NULL);
}
#endif /* CONFIG_IDF_TARGET_ESP32S3 */

/* ── Public API ─────────────────────────────────────────────────────────── */
#if CONFIG_IDF_TARGET_ESP32S3
/* The op set the interpreter needs — built once, referenced by every loaded
 * model (the interpreter keeps a reference, so it must outlive the interp).
 * Ops verified against the reference model (mobilenet_v2 iNat-birds) plus PAD,
 * which Keras-built MobileNetV2 emits as ZeroPadding2D before its stride-2
 * convs (§3.2.2 retrain path). The stock Coral model needs only 6; PAD is
 * harmless there and just makes locally-retrained models load too. */
static tflite::MicroMutableOpResolver<7> &get_resolver(void)
{
    static tflite::MicroMutableOpResolver<7> r;
    static bool init = false;
    if (!init) {
        r.AddAdd();
        r.AddAveragePool2D();
        r.AddConv2D();
        r.AddDepthwiseConv2D();
        r.AddFullyConnected();
        r.AddPad();
        r.AddSoftmax();
        init = true;
    }
    return r;
}

/* Tear down the loaded model but KEEP the tensor arena (its 3 MB PSRAM block is
 * reused by the next model — see model_load_named). Frees the score scratch too
 * so it's re-sized for the next model's class count. Caller holds s_run_mtx (or
 * runs before any task starts). */
static void model_unload(void)
{
    if (s_interp)     { delete s_interp;   s_interp = NULL; }
    if (s_model_buf)  { free(s_model_buf); s_model_buf = NULL; }
    if (s_label_buf)  { free(s_label_buf); s_label_buf = NULL; }
    if (s_labels)     { free(s_labels);    s_labels = NULL; }
    if (s_scores)     { free(s_scores);    s_scores = NULL; }
    if (s_scores_flip){ free(s_scores_flip); s_scores_flip = NULL; }
    s_label_count = 0;
    s_region_matches = 0;
    s_model_name[0] = '\0';
    /* s_arena is intentionally retained. */
}

/* Load model `name` from /sd/model into the (reused) arena, ready to infer.
 * Allocates the arena on first use. Does NOT create the run mutex (one-time in
 * model_init). Assumes no model is currently loaded (call model_unload first
 * when swapping). Returns false (leaving nothing usable) on any failure. */
static bool model_load_named(const char *name)
{
    char p[96];
    snprintf(p, sizeof(p), STORAGE_MOUNT_POINT "/model/%.48s", name);
    size_t model_len = 0;
    s_model_buf = load_file_psram(p, &model_len);
    if (!s_model_buf) {
        ESP_LOGE(TAG, "failed to load %s", p);
        return false;   /* degrade, don't brick (same posture as camera/SD) */
    }
    if (!load_labels(name)) {
        ESP_LOGE(TAG, "no labels file for %s (expected same name .txt)", name);
        free(s_model_buf); s_model_buf = NULL;
        return false;
    }

    const tflite::Model *model = tflite::GetModel(s_model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema v%lu != supported v%d",
                 (unsigned long) model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    if (!s_arena) {
        s_arena = (uint8_t *) heap_caps_malloc(CLS_ARENA_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_arena) {
            ESP_LOGE(TAG, "no PSRAM for tensor arena");
            return false;
        }
    }

    s_interp = new tflite::MicroInterpreter(model, get_resolver(), s_arena, CLS_ARENA_BYTES);
    if (s_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena %d KB, unsupported op, or bad model)",
                 CLS_ARENA_BYTES / 1024);
        return false;
    }
    TfLiteTensor *in = s_interp->input(0);
    if (in->type != kTfLiteInt8 || in->dims->size != 4 ||
        in->dims->data[1] != CLS_INPUT_SIDE || in->dims->data[2] != CLS_INPUT_SIDE ||
        in->dims->data[3] != 3) {
        ESP_LOGE(TAG, "unexpected input tensor (want int8 1x224x224x3)");
        return false;
    }
    strlcpy(s_model_name, name, sizeof(s_model_name));

    ESP_LOGI(TAG, "model loaded: %s (%u KB, %d labels, arena used %u KB)",
             s_model_name, (unsigned) (model_len / 1024), s_label_count,
             (unsigned) (s_interp->arena_used_bytes() / 1024));
    return true;
}

/* Bring up the on-device model. Split out of classify_init so the event task
 * can exist without it: with Claude on (§3.2.3) a card may carry no .tflite at
 * all, and the toggle is live-settable, so the queue/task must be there either
 * way. Returns false (degraded, never fatal) on any missing/bad-model path. */
static bool model_init(void)
{
    if (!storage_sd_present()) {
        ESP_LOGW(TAG, "no SD card — species ID disabled (model lives on SD, §3.2)");
        return false;
    }
    char name[48];
    if (!pick_model_file(name, sizeof(name))) {
        ESP_LOGW(TAG, "no .tflite model in /sd/model — species ID disabled "
                      "(see docs/MODEL.md, or POST /model/upload)");
        return false;
    }
    s_run_mtx = xSemaphoreCreateMutex();
    if (!s_run_mtx) {
        ESP_LOGE(TAG, "mutex create failed — disabled");
        return false;
    }
    return model_load_named(name);
}

/* ── Optional periodic iNat batch (§3.2.3, third tier) ───────────────────────
 * A background booster: every inat_periodic_interval_min, temporarily swap the
 * active (nordic) model for the on-SD iNaturalist model and re-run it, WHOLE-
 * frame (iNat is not ROI-trained — "zoom hurts"), on the frames the primary +
 * Claude left unlabelled, restricted to the 30 target species. Off by default:
 * iNat under-performs on this domain, and a pass holds the run mutex (blocking
 * live ID) for its duration, so it's strictly opt-in and bounded. Frames beyond
 * the per-cycle cap are simply picked up next cycle. */
#define INAT_MODEL_FILE  "inat-birds-v1.tflite"
#define INAT_BATCH_MAX   25          /* frames reclassified per cycle (bounds the swap window) */

/* Score one JPEG whole-frame with the CURRENT model, returning the highest-
 * scoring class whose binomial is one of the 30 target species (§30-mask).
 * Fills common/latin (parsed from the "Latin (Common)" label) + *pct. Returns
 * false on decode/infer error or when no target class appears. Holds no lock —
 * the caller owns s_run_mtx for the whole batch. */
static bool inat_score_target(const uint8_t *jpeg, size_t len,
                              char *common, size_t csz,
                              char *latin, size_t lsz, int *pct)
{
    if (!s_scores) {
        s_scores = (float *) heap_caps_malloc(s_label_count * sizeof(float),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scores) return false;
    }
    int n = 0;
    int32_t dur = 0;
    if (infer_scores(jpeg, len, roi_none(), false, s_scores, &n, &dur) != ESP_OK)
        return false;
    s_last_ms = dur;

    int best = -1;
    for (int i = 0; i < n; i++) {
        const char *lbl = s_labels[i];
        const char *open = strrchr(lbl, '(');
        if (!open) continue;
        char lat[64];
        size_t l = (size_t) (open - lbl);
        while (l > 0 && lbl[l - 1] == ' ') l--;
        if (l >= sizeof(lat)) l = sizeof(lat) - 1;
        memcpy(lat, lbl, l); lat[l] = '\0';
        if (!species_in_target(lat)) continue;
        if (best < 0 || s_scores[i] > s_scores[best]) best = i;
    }
    if (best < 0) return false;

    float p = s_scores[best] < 0 ? 0 : s_scores[best];
    *pct = (int) (p * 100.0f + 0.5f);

    const char *lbl = s_labels[best];
    const char *open = strrchr(lbl, '(');
    size_t l = (size_t) (open - lbl);
    while (l > 0 && lbl[l - 1] == ' ') l--;
    if (l >= lsz) l = lsz - 1;
    memcpy(latin, lbl, l); latin[l] = '\0';
    const char *cp = strchr(open, ')');
    size_t cl = cp ? (size_t) (cp - (open + 1)) : strlen(open + 1);
    if (cl >= csz) cl = csz - 1;
    memcpy(common, open + 1, cl); common[cl] = '\0';
    for (char *c = common; *c; c++) if (*c == ',') *c = ';';   /* CSV-safe */
    return true;
}

static void inat_batch_task(void *arg)
{
    (void) arg;
    /* Tick in short increments (not one long delay) so enabling the feature or
     * changing the interval takes effect within ~30 s instead of only when a
     * boot-time delay expires. Flipping the toggle on schedules the first pass
     * immediately (elapsed primed to the interval); off resets the clock. */
    uint32_t elapsed_s = 0;
    bool was_enabled = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        elapsed_s += 30;

        bool enabled = g_settings.inat_periodic_enabled;
        uint32_t mins = g_settings.inat_periodic_interval_min;
        if (mins < 5) mins = 60;
        uint32_t interval_s = mins * 60;

        if (!enabled) { was_enabled = false; elapsed_s = 0; continue; }
        if (enabled && !was_enabled) elapsed_s = interval_s;   /* just enabled → run next tick */
        was_enabled = true;
        if (elapsed_s < interval_s) continue;
        elapsed_s = 0;

        if (!s_available || !storage_sd_present()) continue;
        if (s_rc_busy) continue;                                  /* manual recheck running */
        if (s_jobq && uxQueueMessagesWaiting(s_jobq) > 0) continue; /* live events queued — yield */

        struct stat stt;
        char ip[96];
        snprintf(ip, sizeof(ip), STORAGE_MOUNT_POINT "/model/" INAT_MODEL_FILE);
        if (stat(ip, &stt) != 0) {
            ESP_LOGW(TAG, "iNat batch: " INAT_MODEL_FILE " not on card — skipping");
            continue;
        }

        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        if (tmv.tm_year + 1900 < 2020) continue;   /* pre-SNTP clock guard (§3.4) */
        char date[11];
        strftime(date, sizeof(date), "%Y-%m-%d", &tmv);

        /* Collect today's still-unclassified, un-corrected rows (cap per cycle). */
        recheck_row_t *rows = (recheck_row_t *) heap_caps_calloc(
            INAT_BATCH_MAX, sizeof(recheck_row_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rows) continue;
        char csv[64];
        storage_visit_log_path(date, csv, sizeof(csv));
        char match[48];
        snprintf(match, sizeof(match), "/captures/%.10s/", date);
        int nc = 0;
        FILE *fp = fopen(csv, "r");
        if (fp) {
            char line[400];
            bool header = true;
            while (fgets(line, sizeof(line), fp) && nc < INAT_BATCH_MAX) {
                if (header) { header = false; continue; }
                if (line[0] == '\0' || line[0] == '\n') continue;
                char *p = line;
                char *ts        = rc_next_field(&p);
                char *species   = rc_next_field(&p);
                rc_next_field(&p);                    /* confidence */
                char *frames    = rc_next_field(&p);
                char *first     = rc_next_field(&p);
                char *corrected = rc_next_field(&p);
                rc_next_field(&p);                    /* latin */
                char *roi_s     = rc_next_field(&p);
                if (corrected[0] || !first[0]) continue;           /* human label wins */
                if (strcmp(species, "unclassified") != 0 &&
                    strcmp(species, "Unidentified bird") != 0) continue;
                if (!strstr(first, match)) continue;               /* today's captures only */
                recheck_row_t *r = &rows[nc];
                strlcpy(r->ts, ts, sizeof(r->ts));
                strlcpy(r->path, first, sizeof(r->path));
                r->frames = atoi(frames);
                r->roi = roi_none();
                float x0, y0, x1, y1;
                if (sscanf(roi_s, "%f-%f-%f-%f", &x0, &y0, &x1, &y1) == 4) {
                    r->roi.x0 = x0; r->roi.y0 = y0; r->roi.x1 = x1; r->roi.y1 = y1;
                }
                r->add = false;
                nc++;
            }
            fclose(fp);
        }
        if (nc == 0) { free(rows); continue; }

        /* Swap nordic -> iNat, holding the run mutex for the whole (bounded)
         * batch so no live inference runs against the wrong model. Restore
         * nordic no matter what before releasing. */
        char prev[48];
        strlcpy(prev, s_model_name, sizeof(prev));
        xSemaphoreTake(s_run_mtx, portMAX_DELAY);
        model_unload();
        if (!model_load_named(INAT_MODEL_FILE)) {
            ESP_LOGE(TAG, "iNat batch: load failed — restoring %s", prev);
            model_unload();
            if (!model_load_named(prev))
                ESP_LOGE(TAG, "iNat batch: restore of %s FAILED — species ID down until reboot", prev);
            xSemaphoreGive(s_run_mtx);
            free(rows);
            continue;
        }

        ESP_LOGI(TAG, "iNat batch: %d candidate frame(s) for %s", nc, date);
        int relabelled = 0;
        for (int i = 0; i < nc; i++) {
            if (!g_settings.inat_periodic_enabled) break;   /* turned off mid-run */
            char full[128];
            snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", rows[i].path);
            size_t len = 0;
            uint8_t *buf = load_file_psram(full, &len);
            if (!buf) continue;
            char common[64], latin[64];
            int pct = 0;
            bool ok = inat_score_target(buf, len, common, sizeof(common),
                                        latin, sizeof(latin), &pct);
            free(buf);
            if (!ok || pct < g_settings.confidence_pct) continue;   /* leave unclassified */
            char roi_s[24] = "";
            if (!roi_is_empty(rows[i].roi))
                snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                         rows[i].roi.x0, rows[i].roi.y0, rows[i].roi.x1, rows[i].roi.y1);
            char nl[400];
            snprintf(nl, sizeof(nl), "%s,%s,%d,%d,%s,,%s,%s,,inat",
                     rows[i].ts, common, pct, rows[i].frames, rows[i].path, latin, roi_s);
            if (rc_rewrite_row(csv, &rows[i], nl)) relabelled++;
        }

        model_unload();
        if (!model_load_named(prev))
            ESP_LOGE(TAG, "iNat batch: restore of %s FAILED — species ID down until reboot", prev);
        xSemaphoreGive(s_run_mtx);
        free(rows);
        ESP_LOGI(TAG, "iNat batch: %d/%d frame(s) got a target-species label", relabelled, nc);
    }
}
#endif

esp_err_t classify_init(void)
{
#if !CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGW(TAG, "species ID unavailable on this target (FSD §3.2) — captures stay 'unclassified'");
    return ESP_OK;
#else
    s_available = model_init();

    /* The event task/queue come up even with no usable model: Claude (§3.2.3)
     * is a live-settable toggle, so a card with no .tflite must still be able
     * to queue and label events the moment the user turns it on — no reboot.
     * With neither classifier, classify_submit_event() declines and capture.c
     * writes its "unclassified" row exactly as before, so the task simply
     * never receives a job.
     *
     * 16 deep (~7.5 KB): a best-of-3 job runs ~13 s (3 x ~4 s inference)
     * while a busy visit produces an event every ~5 s — at 2 deep most of
     * those overflowed to the "unclassified" fallback row. Classification is
     * async and a minutes-deep backlog is acceptable (rows are timestamped
     * with the event time, not the write time), so buffer a whole busy visit
     * rather than dropping events. */
    s_jobq = xQueueCreate(16, sizeof(cls_job_t));
    /* 16 KB stack: TFLM itself fits in 12 KB, but a Claude job runs an mbedTLS
     * handshake on this same task and that alone wants ~5 KB. */
    if (!s_jobq ||
        xTaskCreatePinnedToCore(classify_task, "classify", 16384, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "task/queue create failed — species ID disabled");
        s_available = false;
        return ESP_OK;
    }

    /* Optional periodic iNat batch (§3.2.3, third tier). The task always exists
     * (its interval loop is cheap) and gates on inat_periodic_enabled each wake,
     * so the toggle is live — no reboot. Priority 2, below the event task (3),
     * and it also yields when live events are queued. Needs the on-device model
     * path (it swaps against s_arena / s_run_mtx), so skip when that's down. */
    if (s_available)   /* 14 KB: TFLM Invoke wants ~12 KB (cf. recheck_task) + this task's file-scan locals */
        xTaskCreatePinnedToCore(inat_batch_task, "inat_batch", 14336, NULL, 2, NULL, 1);
    return ESP_OK;
#endif
}

/* On-device model only — /api/classify and /api/classify-file run TFLM
 * synchronously and are unrelated to the cloud path, which has its own
 * cloud_enabled() gate and its own endpoint. */
bool classify_available(void) { return s_available; }

bool classify_submit_event(const char (*paths)[96], const roi_t *rois,
                           int path_count, const char *ts, int frames,
                           const char *first_path)
{
#if CONFIG_IDF_TARGET_ESP32S3
    /* Either classifier will do — with a cloud provider on, events are labelled
     * with no model on the card at all (§3.2.3). Only when neither is available
     * does this decline, leaving capture.c to write the "unclassified" row. */
    bool ondev = s_available && g_settings.ondevice_enabled;
    if ((!ondev && !cloud_enabled() && !inat_cv_enabled()) ||
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
    /* A full queue means every buffered slot is a real visit waiting for a
     * label — giving up here writes a permanent "unclassified" row for a
     * moment of transient pressure. Wait for a slot instead: one in-flight
     * best-of-3 job takes ~13 s, so 15 s spans it. This runs in the motion
     * task after the event's frames are safely on SD; the cost of blocking
     * is a longer cooldown, not lost images. Bounded so a wedged classifier
     * can't stall capture forever. */
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
    if (!s_available || !storage_sd_present()) return false;
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

esp_err_t classify_run_sync(const uint8_t *jpeg, size_t len, classify_result_t *out)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_available) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_run_mtx, portMAX_DELAY);
    esp_err_t err = run_locked(jpeg, len, out, roi_none(), NULL);  /* manual: whole frame */
    xSemaphoreGive(s_run_mtx);
    return err;
#else
    (void) jpeg; (void) len; (void) out;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int32_t     classify_last_duration_ms(void) { return s_last_ms; }
const char *classify_model_name(void)       { return s_model_name; }
int         classify_label_count(void)      { return s_label_count; }
int         classify_region_matches(void)   { return s_region_matches; }
const char *classify_last_species(void)     { return s_last_species; }
const char *classify_last_latin(void)       { return s_last_latin; }
uint8_t     classify_last_confidence(void)  { return s_last_conf; }

const char *classify_label(int i)
{
    return (i >= 0 && i < s_label_count) ? s_labels[i] : "";
}

/* Whether label i would survive the region filter — used to offer the local
 * subset first in the relabel picker (§3.4/v1.51). When the model isn't the
 * Northern-European set (s_region_matches == 0) every label counts as
 * in-region so the picker is never empty. */
bool classify_label_region(int i)
{
    if (i < 0 || i >= s_label_count) return false;
    if (s_region_matches == 0) return true;
    return label_in_region(s_labels[i]);
}
