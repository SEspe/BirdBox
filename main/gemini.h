#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "classify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cloud species identification via the Google Gemini API (FSD §3.2.3).
 *
 * The second cloud classifier, a coequal alternative to claude.c: same job,
 * same classify_result_t output contract, same species-decision rules (see
 * cloud_util.c). Only one cloud provider is active at a time — the selector
 * lives in Settings (cloud_provider) and is dispatched by cloud.c. This module
 * knows only how to talk to Gemini's generateContent endpoint.
 *
 * Off by default. Requires WiFi; every call costs money against the user's own
 * Google AI Studio (Gemini API) key. Uses gemini-2.5-flash — fast and cheap,
 * which suits a per-event secondary bird ID where the bill adds up. */

/* True when a Gemini key is stored — i.e. a Gemini call is possible at all.
 * Mirrors claude_have_key(); gates the manual Gallery button independent of the
 * live provider selection. */
bool gemini_have_key(void);

/* Identify the bird in a JPEG. Blocks the caller for the round-trip
 * (~2-8 s: TLS + upload + inference), so never call from the motion or capture
 * task — the classifier task is the intended caller. Same species contract as
 * claude_classify_jpeg (no bird / Unidentified bird / species+binomial), so an
 * inconclusive answer can never overwrite a good label downstream.
 *
 * Returns ESP_ERR_INVALID_STATE when no key, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT,
 * or ESP_FAIL on a transport/parse error. */
esp_err_t gemini_classify_jpeg(const uint8_t *jpeg, size_t len,
                               classify_result_t *out);

/* Same, for a JPEG already on the SD card. */
esp_err_t gemini_classify_file(const char *fs_path, classify_result_t *out);

/* Check reachability + the stored key without spending tokens: resolves the API
 * host, probes TCP 443, then GETs the free ListModels endpoint. Writes a
 * human-readable verdict into `out`. Returns ESP_OK only when the key works. */
esp_err_t gemini_test(char *out, size_t osz);

/* Debug-card data (FSD §5) — last call's outcome. */
const char *gemini_last_error(void);      /* "" when the last call succeeded */
int32_t     gemini_last_duration_ms(void);/* -1 = no call yet */
uint32_t    gemini_call_count(void);      /* successful calls since boot */

#ifdef __cplusplus
}
#endif
