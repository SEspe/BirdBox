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

/* Best-of-N: how many of an event's saved frames the classifier scores,
 * keeping the most confident real-species result (§3.2). The event's first
 * frame is often the worst (bird mid-entry / motion-blurred), so scoring a
 * few and picking the best improves the label. */
#define CLASSIFY_BEST_OF_N 3

/* Queues a visit event for async best-of-N classification + visit-log write.
 * `paths` are the event's saved-frame paths (web-relative, e.g.
 * "/captures/DAY/NAME.jpg"), best-first; up to CLASSIFY_BEST_OF_N are scored,
 * the rest ignored. The classifier re-reads each from SD. All args are
 * copied; the caller keeps ownership. Returns false (classifier disabled or
 * queue full) so the caller can write the fallback "unclassified" row. */
bool classify_submit_event(const char (*paths)[96], int path_count,
                           const char *ts, int frames, const char *first_path);

/* Synchronous one-shot classification (POST /api/classify). Blocks the
 * caller for the full decode + inference (~seconds); shares a lock with
 * the event task. */
esp_err_t classify_run_sync(const uint8_t *jpeg, size_t len,
                            classify_result_t *out);

/* Debug-card data (FSD §5) */
int32_t     classify_last_duration_ms(void);   /* -1 = none yet */
const char *classify_model_name(void);         /* "" when no model loaded */
int         classify_label_count(void);
int         classify_region_matches(void);     /* loaded labels in the N-Euro
                                                  set; 0 = region filter N/A */
const char *classify_last_species(void);       /* last event decision, "" */
const char *classify_last_latin(void);         /* matching binomial, "" */

#ifdef __cplusplus
}
#endif
