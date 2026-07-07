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
    char    species[64];        /* decision: common name, "Unidentified bird"
                                   or "no bird" (threshold + guard applied) */
    uint8_t confidence_pct;     /* top-1 confidence */
    int32_t duration_ms;        /* inference (Invoke) time */
} classify_result_t;

/* Queues one visit event's best frame for async classification + visit-log
 * write. On true, ownership of jpeg (heap buffer) transfers to the
 * classifier; on false the caller still owns it and must write the
 * fallback log row itself. ts/first_path are copied. */
bool classify_submit_event(uint8_t *jpeg, size_t len, const char *ts,
                           int frames, const char *first_path);

/* Synchronous one-shot classification (POST /api/classify). Blocks the
 * caller for the full decode + inference (~seconds); shares a lock with
 * the event task. */
esp_err_t classify_run_sync(const uint8_t *jpeg, size_t len,
                            classify_result_t *out);

/* Debug-card data (FSD §5) */
int32_t     classify_last_duration_ms(void);   /* -1 = none yet */
const char *classify_model_name(void);         /* "" when no model loaded */
int         classify_label_count(void);
const char *classify_last_species(void);       /* last event decision, "" */

#ifdef __cplusplus
}
#endif
