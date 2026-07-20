#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "classify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cloud species identification via the iNaturalist Computer Vision API
 * (FSD §3.2.3) — the online `POST /v1/computervision/score_image` model that
 * powers the iNat app: current, geo/region-capable, ~100k taxa. This is NOT the
 * weak on-device `inat-birds-v1.tflite` (965-class, domain-mismatched); the
 * online model is far stronger on this camera's Nordic feeder birds.
 *
 * Runs as the PRIMARY classification tier when enabled (see classify.cpp's
 * cascade: iNat online → nordic-v0.x → Claude/Gemini), because it's free (no
 * per-call fee, unlike the Anthropic/Google tiers).
 *
 * AUTH CAVEAT: the endpoint needs a JWT (`Authorization: Bearer <token>`), and
 * an iNat JWT EXPIRES ~24 h after issue. There is no permanent API key. The
 * token is pasted in Settings (grabbed from inaturalist.org/users/api_token);
 * for unattended operation it must be refreshed — an OAuth auto-refresh is a
 * possible follow-up. Results returned in classify_result_t so every existing
 * consumer works unchanged. */

/* True when a JWT is stored — a call is possible at all. */
bool inat_have_token(void);

/* True when the CV tier is switched on AND a token is stored: identify new
 * motion events with iNat online first. Cheap; the live-cascade primary gate. */
bool inat_cv_enabled(void);

/* Call after storing a new token so any leftover rate-limit cooldown / error is
 * cleared and the fresh token is used on the next event (no reboot). */
void inat_token_changed(void);

/* Identify the bird in a JPEG via score_image. Blocks the caller for the
 * round-trip (~1-4 s: TLS + multipart upload + inference). Same species-decision
 * contract as classify.cpp: a confident bird species fills species+latin; an
 * unsure/low-score result yields "Unidentified bird" (empty latin) so it falls
 * through to the nordic tier; iNat has no reliable "no bird" so that is left to
 * nordic's background class. Returns ESP_ERR_INVALID_STATE (no token),
 * ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT, or ESP_FAIL. */
esp_err_t inat_classify_jpeg(const uint8_t *jpeg, size_t len, classify_result_t *out);

/* Same, for a JPEG already on the SD card. */
esp_err_t inat_classify_file(const char *fs_path, classify_result_t *out);

/* Validate reachability + the stored token WITHOUT a CV call: resolves the host,
 * probes TCP 443, then GETs the free /v1/users/me endpoint. Writes a
 * human-readable verdict into `out` ("token valid - user <login>", "token
 * rejected/expired", ...). Returns ESP_OK only when the token works. */
esp_err_t inat_test(char *out, size_t osz);

/* Seconds of rate-limit (429) cooldown remaining, 0 = none. While > 0, live
 * events skip iNat (fall through) instead of hammering the throttle. */
int inat_cooldown_s(void);

/* Debug-card data (FSD §5). */
const char *inat_last_error(void);       /* "" when the last call succeeded */
int32_t     inat_last_duration_ms(void); /* -1 = no call yet */
uint32_t    inat_call_count(void);       /* successful calls since boot */

#ifdef __cplusplus
}
#endif
