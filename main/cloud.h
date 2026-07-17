#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "classify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cloud classifier dispatcher (FSD §3.2.3). BirdBox can use one cloud vision
 * model as an optional SECONDARY classifier behind the on-device model — either
 * Anthropic Claude (claude.c) or Google Gemini (gemini.c). Both can hold a
 * stored key at the same time, but only ONE is active at a time: the operator
 * picks it with the cloud_provider selector in Settings. This module resolves
 * that selection and routes the live cascade + the UI to the chosen provider so
 * callers never branch on it themselves. */

typedef enum {
    CLOUD_OFF    = 0,   /* no cloud classifier — on-device model only */
    CLOUD_CLAUDE = 1,
    CLOUD_GEMINI = 2,
} cloud_provider_t;

/* The provider selected AND currently usable: g_settings.cloud_provider, but
 * downgraded to CLOUD_OFF if that provider has no key stored (so a stale
 * selection with a forgotten key never claims to be live). */
cloud_provider_t cloud_active(void);

/* True when a cloud classifier should identify NEW motion events — i.e.
 * cloud_active() != CLOUD_OFF. Cheap; the live cascade gate. A call that fails
 * degrades to the on-device answer at the call site, never a dropped event. */
bool cloud_enabled(void);

/* Whether a given provider has a key stored (a call is possible at all). */
bool cloud_have_key(cloud_provider_t p);

/* Display name ("Claude"/"Gemini"/"none") and the visit-log source tag
 * ("claude"/"gemini"/"") for a provider. */
const char *cloud_name(cloud_provider_t p);
const char *cloud_source_tag(cloud_provider_t p);

/* Provider for a MANUAL Gallery identify: the active one if any, else whichever
 * single provider has a key (so the ✨ button works with the live path off, as
 * the Claude-only button used to). CLOUD_OFF only when no key is stored at all. */
cloud_provider_t cloud_for_manual(void);

/* Identify a saved JPEG with a specific provider (routes to claude/gemini
 * _classify_file). Used by the live cascade with cloud_active() and by the
 * manual button with cloud_for_manual(). */
esp_err_t cloud_classify_file(cloud_provider_t p, const char *fs_path,
                              classify_result_t *out);

/* Reachability/key probe for a specific provider; verdict into `out`. */
esp_err_t cloud_test(cloud_provider_t p, char *out, size_t osz);

/* Last-call error string for a provider (for the Debug card / API replies). */
const char *cloud_last_error(cloud_provider_t p);

#ifdef __cplusplus
}
#endif
