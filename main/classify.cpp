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
static esp_err_t decode_to_input(const uint8_t *jpeg, size_t len, uint8_t *dst224, rotation_t rot)
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

    /* Halve while both dims stay >= the input side and the buffer stays
     * within budget — SVGA 800x600 -> 1/2 (400x300), a 960px photo -> 1/2. */
    int scale = 0, w = info.width, h = info.height;
    while (scale < 3 &&
           ((w >> 1) >= CLS_INPUT_SIDE && (h >> 1) >= CLS_INPUT_SIDE)) {
        w >>= 1; h >>= 1; scale++;
    }
    while (scale < 3 && (size_t) w * h * 3 > CLS_DECODE_MAX) {
        w >>= 1; h >>= 1; scale++;
    }
    if ((size_t) w * h * 3 > CLS_DECODE_MAX) {
        ESP_LOGW(TAG, "image too large to decode (%ux%u)", info.width, info.height);
        return ESP_ERR_NO_MEM;
    }

    size_t   out_size = (size_t) w * h * 3 + 16;
    uint8_t *rgb = (uint8_t *) heap_caps_malloc(out_size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) return ESP_ERR_NO_MEM;

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

    /* Center-crop to square, nearest-neighbor resize to 224x224 */
    int side = w < h ? w : h;
    int x0 = (w - side) / 2, y0 = (h - side) / 2;
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
static esp_err_t run_locked(const uint8_t *jpeg, size_t len, classify_result_t *out)
{
    memset(out, 0, sizeof(*out));
    TfLiteTensor *in = s_interp->input(0);

    esp_err_t err = decode_to_input(jpeg, len, in->data.uint8, g_settings.rotation);
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
static esp_err_t run_one(const uint8_t *jpeg, size_t len, classify_result_t *out)
{
    xSemaphoreTake(s_run_mtx, portMAX_DELAY);
    esp_err_t err = run_locked(jpeg, len, out);
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

/* ── Event task: best-of-N classify, then write the visit-log row (async) ── */
static void classify_task(void *arg)
{
    cls_job_t job;
    for (;;) {
        if (xQueueReceive(s_jobq, &job, portMAX_DELAY) != pdTRUE) continue;

        classify_result_t best;
        bool have_best = false;
        int  scored = 0;
        for (int i = 0; i < job.path_count; i++) {
            char full[128];
            snprintf(full, sizeof(full), STORAGE_MOUNT_POINT "%s", job.paths[i]);
            size_t len = 0;
            uint8_t *buf = load_file_psram(full, &len);
            if (!buf) { ESP_LOGW(TAG, "best-of: read %s failed", full); continue; }
            classify_result_t r;
            esp_err_t err = run_one(buf, len, &r);
            free(buf);
            if (err != ESP_OK) continue;
            scored++;
            if (!have_best || result_better(&r, &best)) { best = r; have_best = true; }
        }

        char line[288];
        if (have_best) {
            strlcpy(s_last_species, best.species, sizeof(s_last_species));
            strlcpy(s_last_latin, best.latin, sizeof(s_last_latin));
            ESP_LOGI(TAG, "event @%s: %s (%u%%, top1 '%s', best of %d/%d frame(s), %ld ms)",
                     job.ts, best.species, best.confidence_pct, best.top_label[0],
                     scored, job.path_count, (long) best.duration_ms);
            snprintf(line, sizeof(line), "%s,%s,%u,%d,%s,,%s",
                     job.ts, best.species, best.confidence_pct, job.frames, job.first_path,
                     best.latin);
        } else {
            snprintf(line, sizeof(line), "%s,unclassified,0,%d,%s,,",
                     job.ts, job.frames, job.first_path);
        }
        if (storage_append_visit_log(line) != ESP_OK)
            ESP_LOGW(TAG, "visit log append failed");
    }
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
    s_jobq    = xQueueCreate(2, sizeof(cls_job_t));
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

bool classify_submit_event(const char (*paths)[96], int path_count,
                           const char *ts, int frames, const char *first_path)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_available || path_count <= 0) return false;
    cls_job_t job = {};
    job.frames = frames;
    strlcpy(job.ts, ts, sizeof(job.ts));
    strlcpy(job.first_path, first_path, sizeof(job.first_path));
    int n = path_count < CLASSIFY_BEST_OF_N ? path_count : CLASSIFY_BEST_OF_N;
    for (int i = 0; i < n; i++)
        strlcpy(job.paths[i], paths[i], sizeof(job.paths[0]));
    job.path_count = n;
    if (xQueueSend(s_jobq, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "classify queue full — event logged unclassified");
        return false;
    }
    return true;
#else
    (void) paths; (void) path_count; (void) ts; (void) frames; (void) first_path;
    return false;
#endif
}

esp_err_t classify_run_sync(const uint8_t *jpeg, size_t len, classify_result_t *out)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_available) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_run_mtx, portMAX_DELAY);
    esp_err_t err = run_locked(jpeg, len, out);
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
