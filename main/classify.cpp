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
#include "species_i18n.h"   /* has its own extern "C" guard */
extern "C" {          /* C headers without their own __cplusplus guards */
#include "settings.h"
#include "storage.h"
}

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

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
static volatile int32_t s_last_ms = -1;
static char             s_last_species[64] = "";
static char             s_last_latin[64] = "";

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
                                 rotation_t rot, roi_t roi)
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
                drow[x * 3 + 0] = row[sx * 3 + 0];
                drow[x * 3 + 1] = row[sx * 3 + 1];
                drow[x * 3 + 2] = row[sx * 3 + 2];
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
                drow[x * 3 + 0] = sp[0];
                drow[x * 3 + 1] = sp[1];
                drow[x * 3 + 2] = sp[2];
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

/* ── Inference core (shared by the event task and /api/classify) ────────── */
static esp_err_t run_locked(const uint8_t *jpeg, size_t len, classify_result_t *out, roi_t roi)
{
    memset(out, 0, sizeof(*out));
    TfLiteTensor *in = s_interp->input(0);

    /* Zoom is opt-out (§3.2): an empty roi already means whole-frame, and the
     * setting lets the user disable ROI cropping entirely. */
    if (!g_settings.detect_zoom) roi = roi_none();
    esp_err_t err = decode_to_input(jpeg, len, in->data.uint8, g_settings.rotation, roi);
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
    out->duration_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    s_last_ms = out->duration_ms;

    TfLiteTensor *ot = s_interp->output(0);
    const int8_t *scores = ot->data.int8;
    int n = ot->dims->data[1];
    if (n > s_label_count) n = s_label_count;

    /* Region filter (§3.2.1): restrict the ranking to Northern-European species
     * (plus no-binomial guard labels). Confidence is NOT renormalized — an
     * out-of-region animal simply loses its winning class and the best in-region
     * score falls below the threshold, landing as "Unidentified bird". Skipped
     * when the loaded model doesn't match the set, so it can't blank everything. */
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
    for (int k = 0; k < 3; k++) {
        if (best[k] < 0) break;
        strlcpy(out->top_label[k], s_labels[best[k]], sizeof(out->top_label[k]));
        float p = ((float) scores[best[k]] - ot->params.zero_point) * ot->params.scale;
        out->top_pct[k] = (uint8_t) (p * 100.0f + 0.5f);
    }
    make_decision(out);
    return ESP_OK;
}

/* One inference, taking the run mutex for just this frame (best-of-N loops
 * lock per frame so a manual /api/classify isn't blocked for the whole run). */
static esp_err_t run_one(const uint8_t *jpeg, size_t len, classify_result_t *out, roi_t roi)
{
    xSemaphoreTake(s_run_mtx, portMAX_DELAY);
    esp_err_t err = run_locked(jpeg, len, out, roi);
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

/* ── Event task: best-of-N classify, then write the visit-log row (async) ── */
static void classify_task(void *arg)
{
    cls_job_t job;
    for (;;) {
        if (xQueueReceive(s_jobq, &job, portMAX_DELAY) != pdTRUE) continue;

        classify_result_t best;
        bool  have_best = false;
        int   scored = 0, best_idx = 0;
        bool  zoomed_any = false;
        for (int i = 0; i < job.path_count; i++) {
            char full[128];
            snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job.paths[i]);
            size_t len = 0;
            uint8_t *buf = load_file_psram(full, &len);
            if (!buf) { ESP_LOGW(TAG, "best-of: read %s failed", full); continue; }
            /* "zoom was attempted", not "zoom succeeded" — the rescue below
             * must still fire when every zoomed run failed outright. */
            if (g_settings.detect_zoom && !roi_is_empty(job.rois[i])) zoomed_any = true;
            classify_result_t r;
            esp_err_t err = run_one(buf, len, &r, job.rois[i]);
            free(buf);
            if (err != ESP_OK) continue;
            scored++;
            if (!have_best || result_better(&r, &best)) { best = r; best_idx = i; have_best = true; }
        }

        /* Zoom safety net (§3.2): the ROI crop is a bet that the changed cells
         * contain the bird. If every zoomed frame came back without a real
         * species — or none could be scored at all (e.g. the ROI decode buffer
         * didn't fit in free PSRAM) — spend ONE extra whole-frame inference on
         * the frame that scored best (the event's first frame when nothing
         * scored): a miss-cropped bird is often still identifiable in the full
         * view, and the whole-frame decode is far smaller than a zoomed one.
         * Skipped when nothing was zoomed (retrying the same whole-frame run
         * would fail the same way). */
        if ((!have_best || best.latin[0] == '\0') && zoomed_any) {
            char full[128];
            snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job.paths[best_idx]);
            size_t len = 0;
            uint8_t *buf = load_file_psram(full, &len);
            if (buf) {
                classify_result_t r;
                if (run_one(buf, len, &r, roi_none()) == ESP_OK &&
                    (!have_best || result_better(&r, &best))) {
                    ESP_LOGI(TAG, "whole-frame fallback rescued: %s (%u%%)",
                             r.species, r.confidence_pct);
                    best = r;
                    have_best = true;
                    job.rois[best_idx] = roi_none();   /* log the roi actually used */
                }
                free(buf);
            }
        }

        char line[400];
        if (have_best) {
            strlcpy(s_last_species, best.species, sizeof(s_last_species));
            strlcpy(s_last_latin, best.latin, sizeof(s_last_latin));
            ESP_LOGI(TAG, "event @%s: %s (%u%%, top1 '%s', best of %d/%d frame(s), %ld ms)",
                     job.ts, best.species, best.confidence_pct, best.top_label[0],
                     scored, job.path_count, (long) best.duration_ms);
            /* winning frame's ROI ("x0-y0-x1-y1", empty = whole frame) + top-3
             * as trailing columns — field-tuning data (§3.4) */
            char roi_s[24] = "";
            roi_t ur = job.rois[best_idx];
            if (g_settings.detect_zoom && !roi_is_empty(ur))
                snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                         ur.x0, ur.y0, ur.x1, ur.y1);
            char top3[112];
            top3_field(&best, top3, sizeof(top3));
            snprintf(line, sizeof(line), "%s,%s,%u,%d,%s,,%s,%s,%s",
                     job.ts, best.species, best.confidence_pct, job.frames,
                     job.first_path, best.latin, roi_s, top3);
        } else {
            snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,,,,",
                     job.ts, job.frames, job.first_path);
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
} recheck_row_t;

#define RECHECK_MAX_ROWS 256   /* ~32 kB PSRAM; > any plausible day */

static volatile bool s_rc_busy = false;
static volatile int  s_rc_done = 0, s_rc_total = 0;
static char          s_rc_date[11];

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
    snprintf(csv, sizeof(csv), STORAGE_MOUNT_POINT "/log/visits-%.7s.csv", s_rc_date);

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
    s_rc_total = n;

    for (int i = 0; i < n; i++) {
        recheck_row_t *row = &rows[i];
        char full[128];
        snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", row->path);
        size_t len = 0;
        uint8_t *buf = load_file_psram(full, &len);
        if (!buf) { s_rc_done = i + 1; continue; }   /* photo deleted — keep row */

        classify_result_t r;
        bool ok = run_one(buf, len, &r, row->roi) == ESP_OK;
        /* same whole-frame safety net as live events (§3.2) */
        if (ok && r.latin[0] == '\0' && !roi_is_empty(row->roi)) {
            classify_result_t w;
            if (run_one(buf, len, &w, roi_none()) == ESP_OK && result_better(&w, &r)) {
                r = w;
                row->roi = roi_none();
            }
        }
        free(buf);
        if (ok) {
            char roi_s[24] = "";
            if (g_settings.detect_zoom && !roi_is_empty(row->roi))
                snprintf(roi_s, sizeof(roi_s), "%.2f-%.2f-%.2f-%.2f",
                         row->roi.x0, row->roi.y0, row->roi.x1, row->roi.y1);
            char top3[112];
            top3_field(&r, top3, sizeof(top3));
            char nl[400];
            snprintf(nl, sizeof(nl), "%s,%s,%u,%d,%s,,%s,%s,%s",
                     row->ts, r.species, r.confidence_pct, row->frames,
                     row->path, r.latin, roi_s, top3);
            rc_rewrite_row(csv, row, nl);
        }
        s_rc_done = i + 1;
    }
    free(rows);
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
    if (!storage_sd_present()) {
        ESP_LOGW(TAG, "no SD card — species ID disabled (model lives on SD, §3.2)");
        return ESP_OK;
    }
    if (!pick_model_file(s_model_name, sizeof(s_model_name))) {
        ESP_LOGW(TAG, "no .tflite model in /sd/model — species ID disabled "
                      "(see docs/MODEL.md, or POST /model/upload)");
        return ESP_OK;
    }

    char p[96];
    snprintf(p, sizeof(p), STORAGE_MOUNT_POINT "/model/%.48s", s_model_name);
    size_t model_len = 0;
    s_model_buf = load_file_psram(p, &model_len);
    if (!s_model_buf) {
        ESP_LOGE(TAG, "failed to load %s", p);
        return ESP_OK;   /* degrade, don't brick (same posture as camera/SD) */
    }
    if (!load_labels(s_model_name)) {
        ESP_LOGE(TAG, "no labels file for %s (expected same name .txt) — disabled",
                 s_model_name);
        free(s_model_buf); s_model_buf = NULL;
        return ESP_OK;
    }

    const tflite::Model *model = tflite::GetModel(s_model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema v%lu != supported v%d — disabled",
                 (unsigned long) model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_OK;
    }

    s_arena = (uint8_t *) heap_caps_malloc(CLS_ARENA_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_arena) {
        ESP_LOGE(TAG, "no PSRAM for tensor arena — disabled");
        return ESP_OK;
    }

    /* Ops verified against the reference model (mobilenet_v2 iNat-birds) */
    static tflite::MicroMutableOpResolver<6> resolver;
    resolver.AddAdd();
    resolver.AddAveragePool2D();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    s_interp = new tflite::MicroInterpreter(model, resolver, s_arena, CLS_ARENA_BYTES);
    if (s_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena %d KB, unsupported op, or bad model) — disabled",
                 CLS_ARENA_BYTES / 1024);
        return ESP_OK;
    }
    TfLiteTensor *in = s_interp->input(0);
    if (in->type != kTfLiteInt8 || in->dims->size != 4 ||
        in->dims->data[1] != CLS_INPUT_SIDE || in->dims->data[2] != CLS_INPUT_SIDE ||
        in->dims->data[3] != 3) {
        ESP_LOGE(TAG, "unexpected input tensor (want int8 1x224x224x3) — disabled");
        return ESP_OK;
    }

    s_run_mtx = xSemaphoreCreateMutex();
    /* 16 deep (~7.5 KB): a best-of-3 job runs ~13 s (3 x ~4 s inference)
     * while a busy visit produces an event every ~5 s — at 2 deep most of
     * those overflowed to the "unclassified" fallback row. Classification is
     * async and a minutes-deep backlog is acceptable (rows are timestamped
     * with the event time, not the write time), so buffer a whole busy visit
     * rather than dropping events. */
    s_jobq    = xQueueCreate(16, sizeof(cls_job_t));
    if (!s_run_mtx || !s_jobq ||
        xTaskCreatePinnedToCore(classify_task, "classify", 12288, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "task/queue create failed — disabled");
        return ESP_OK;
    }

    s_available = true;
    ESP_LOGI(TAG, "classifier ready: %s (%u KB, %d labels, arena used %u KB)",
             s_model_name, (unsigned) (model_len / 1024), s_label_count,
             (unsigned) (s_interp->arena_used_bytes() / 1024));
    return ESP_OK;
#endif
}

bool classify_available(void) { return s_available; }

bool classify_submit_event(const char (*paths)[96], const roi_t *rois,
                           int path_count, const char *ts, int frames,
                           const char *first_path)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_available || path_count <= 0) return false;
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

bool classify_recheck_start(const char *date)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_available || !storage_sd_present()) return false;
    if (!date || strlen(date) != 10) return false;
    if (s_rc_busy) return false;
    s_rc_busy  = true;
    s_rc_done  = 0;
    s_rc_total = 0;
    strlcpy(s_rc_date, date, sizeof(s_rc_date));
    /* Priority 2 — below the event task (3), so live visits always win the
     * run mutex first and a recheck only fills the gaps. */
    if (xTaskCreatePinnedToCore(recheck_task, "recheck", 12288, NULL, 2, NULL, 1)
        != pdPASS) {
        s_rc_busy = false;
        return false;
    }
    return true;
#else
    (void) date;
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
    esp_err_t err = run_locked(jpeg, len, out, roi_none());  /* manual: whole frame */
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
