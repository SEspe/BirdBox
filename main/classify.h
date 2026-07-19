#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On-device species identification (FSD §3.2): quantized int8/uint8
 * classifier (TFLite-Micro + esp-nn) on the S3. Asynchronous — capture is
 * never delayed; the classifier task writes the visit-log row when the
 * label is ready. Top-3 + confidence; below threshold -> "Unidentified
 * bird"; "background" guard class -> "no bird". Disabled on ESP32-classic
 * and when no model file is present on SD (captures stay "unclassified"). */

esp_err_t classify_init(void);

/* False when disabled (no S3, no SD, no model file, or init failed) —
 * callers fall back to the pre-§3.2 "unclassified" visit-log path. */
bool classify_available(void);

typedef struct {
    char    top_label[3][64];   /* raw model labels, best first */
    uint8_t top_pct[3];         /* confidence 0-100 */
    char    species[64];        /* decision: English common name,
                                   "Unidentified bird" or "no bird" (threshold
                                   + guard applied) */
    char    latin[64];          /* scientific binomial; empty for the three
                                   sentinel values above (§3.2 language
                                   selection — species_i18n.h always shows
                                   this regardless of display language) */
    uint8_t confidence_pct;     /* top-1 confidence */
    int32_t duration_ms;        /* inference (Invoke) time */
} classify_result_t;

/* Normalized region-of-interest (0..1, origin top-left, source-frame axes)
 * that motion detection found changed — used to "zoom" species ID onto the
 * bird (FSD §3.1/§3.2). An empty rect (x1<=x0 || y1<=y0) means "whole frame":
 * the classifier center-crops as before. */
typedef struct { float x0, y0, x1, y1; } roi_t;
static inline bool  roi_is_empty(roi_t r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static inline roi_t roi_none(void) { roi_t r = {0, 0, 0, 0}; return r; }

/* Frame capacity per event: how many of an event's saved frames the classifier
 * carries and scores. Sized to the max capture_count (settings.c clamps ccnt to
 * 10), so EVERY captured frame is classified — not just the first few (§3.2).
 * The iNat tier scores all of them and requires the winning species to be
 * corroborated by >=2 frames (v2.26); the nordic tier keeps its best-of-N +
 * evidence-pooling over the same set. The event's first frame is often the
 * worst (bird mid-entry / motion-blurred), so scoring all and cross-checking
 * beats trusting any single view. */
#define CLASSIFY_BEST_OF_N 10

/* Queues a visit event for async best-of-N classification + visit-log write.
 * `paths` are the event's saved-frame paths (web-relative, e.g.
 * "/captures/DAY/NAME.jpg"), best-first; up to CLASSIFY_BEST_OF_N (= the max
 * capture_count, i.e. all of them) are scored. The classifier re-reads each
 * from SD. `rois` is the
 * parallel array of per-frame motion regions — species ID zooms each frame
 * onto its own ROI (empty rect -> whole frame; honoured only when
 * g_settings.detect_zoom is on). All args are copied; the caller keeps
 * ownership. Returns false (classifier disabled or queue full) so the caller
 * can write the fallback "unclassified" row. */
bool classify_submit_event(const char (*paths)[96], const roi_t *rois,
                           int path_count, const char *ts, int frames,
                           const char *first_path);

/* Synchronous one-shot classification (POST /api/classify). Blocks the
 * caller for the full decode + inference (~seconds); shares a lock with
 * the event task. */
esp_err_t classify_run_sync(const uint8_t *jpeg, size_t len,
                            classify_result_t *out);

/* Re-check one day (FSD §3.4): re-runs species ID over every visit-log row
 * of `date` ("YYYY-MM-DD") with the current model/settings, rewriting each
 * row in place as it completes (timestamp/frames/first_frame are preserved;
 * species/confidence/latin/roi/top3 are replaced; user-corrected rows are
 * skipped). Asynchronous — runs in its own task, one recheck at a time.
 * `files` optionally narrows the run to rows whose first_frame basename is
 * in the comma-separated list (NULL/empty = the whole day) — the Gallery's
 * recheck-selected, since a full day takes ~5-10 s per row. Returns false
 * when the classifier is unavailable, no SD, or a recheck is already
 * running; a date with no (matching) rows finishes immediately (total 0). */
bool classify_recheck_start(const char *date, const char *files);

/* Progress of the current (busy=true) or most recent recheck. `date` may be
 * NULL; done/total are 0 before the row scan finishes. */
void classify_recheck_status(bool *busy, int *done, int *total,
                             char *date, size_t date_len);

/* Debug-card data (FSD §5) */
int32_t     classify_last_duration_ms(void);   /* -1 = none yet */
const char *classify_model_name(void);         /* "" when no model loaded */
int         classify_label_count(void);
int         classify_region_matches(void);     /* loaded labels in the N-Euro
                                                  set; 0 = region filter N/A */
const char *classify_last_species(void);       /* last event decision, "" */
const char *classify_last_latin(void);         /* matching binomial, "" */
uint8_t     classify_last_confidence(void);    /* confidence % of last event */

/* Model label access for the relabel picker (FSD §3.4/v1.51): raw label at
 * index i ("Latin (Common)"), "" out of range; and whether it's in the
 * Northern-European region subset (all labels when the model isn't that set). */
const char *classify_label(int i);
bool        classify_label_region(int i);

#ifdef __cplusplus
}
#endif
