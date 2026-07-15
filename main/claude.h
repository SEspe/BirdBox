#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "classify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cloud species identification via the Anthropic Claude API (FSD §3.2.3).
 *
 * Opt-in alternative to the on-device TFLM classifier: when enabled in
 * Settings (toggle + API key), new motion events are identified by Claude
 * instead of the local model, and the Gallery grows a per-image button to ask
 * Claude about one saved photo. Purpose is the §3.2.2 retrain loop — Claude is
 * a far stronger labeller than the v1 iNat model, so it makes building a
 * Nordic training set a click-to-confirm job instead of a typing job.
 *
 * Results are returned in classify_result_t so every existing consumer (the
 * visit-log row writer, classify_send_json, the confidence threshold, the
 * "empty latin => never write a label" guard) works unchanged. Claude fills
 * top_label[0]/top_pct[0] only — it returns one answer, not a top-3.
 *
 * Off by default. Requires WiFi; every call costs money against the user's
 * own Anthropic API key. */

/* True when a key is stored — i.e. a Claude call is possible at all. This, not
 * claude_enabled(), gates the manual Gallery button: labelling a training image
 * on demand shouldn't require turning on billing for every live event. */
bool claude_have_key(void);

/* True when the Settings toggle is on AND a key is stored: identify NEW MOTION
 * EVENTS with Claude instead of the local model. Governs the live path only.
 * Cheap — safe to call per event. Does not check WiFi (a call that fails
 * degrades to the on-device answer at the call site). */
bool claude_enabled(void);

/* Identify the bird in a JPEG. Blocks the caller for the round-trip
 * (~2-8 s: TLS handshake + upload + inference), so never call from the motion
 * or capture task — the classifier task is the intended caller.
 *
 * The species decision follows the same contract as classify.cpp:
 *   • no bird in frame        -> species "no bird",          latin ""
 *   • bird, below threshold   -> species "Unidentified bird", latin ""
 *   • bird, above threshold   -> species <English common>,    latin <binomial>
 * so an inconclusive answer can never overwrite a good label downstream.
 *
 * Returns ESP_ERR_INVALID_STATE when no key, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT,
 * or ESP_FAIL on a transport/parse error. */
esp_err_t claude_classify_jpeg(const uint8_t *jpeg, size_t len,
                               classify_result_t *out);

/* Same, for a JPEG already on the SD card. `fs_path` is an absolute FATFS
 * path ("/sd/captures/DAY/NAME.jpg"); the file is read into PSRAM and handed
 * to claude_classify_jpeg. */
esp_err_t claude_classify_file(const char *fs_path, classify_result_t *out);

/* Check reachability + the stored key without spending tokens: resolves the
 * API host, probes TCP 443, then does a GET of the free /v1/models endpoint.
 * Writes a human-readable verdict into `out` ("key valid - 8 models", "key
 * rejected", "DNS lookup failed", ...). Returns ESP_OK only when the key
 * actually works. There is no serial console on a deployed box, so this is the
 * only way to tell a bad key from a blocked network. Blocks ~1-3 s. */
esp_err_t claude_test(char *out, size_t osz);

/* Debug-card data (FSD §5) — last call's outcome, for diagnosing a key or
 * connectivity problem without a serial cable. */
const char *claude_last_error(void);      /* "" when the last call succeeded */
int32_t     claude_last_duration_ms(void);/* -1 = no call yet */
uint32_t    claude_call_count(void);      /* successful calls since boot */

#ifdef __cplusplus
}
#endif
