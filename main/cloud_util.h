#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_http_client.h"
#include "classify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared plumbing for the cloud species-ID providers (Claude, Gemini — FSD
 * §3.2.3). Both talk JSON-over-HTTPS to a vision model with the same job:
 * base64 a JPEG into a request body, read one schema'd answer back, and turn it
 * into a classify_result_t under the project's species-decision contract.
 *
 * The two providers differ only in URL, auth header, request/response JSON
 * shape and model id. Everything below is byte-identical work that used to live
 * inside claude.c; keeping one copy stops the two request paths from drifting
 * (a comma that shifts the CSV visit log, a TLS verdict that reads as garbage —
 * both are real bugs this code already paid for once). */

/* ── Minimal JSON reader ─────────────────────────────────────────────────────
 * No cJSON in this project (every other reply here is hand-assembled); four
 * field lookups don't justify the heap. These find a key at any depth, which is
 * fine for the shapes read: one text block, and inside it one of each field. */

/* Pointer to the value after "key": , or NULL. Skips a match NOT followed by a
 * colon (that match is a *value*, not a key) — load-bearing because a content
 * block is {"type":"text","text":".."}, so the first "text" in a reply is the
 * value of "type". Stopping there would make every good reply look unparseable. */
const char *cu_json_seek(const char *j, const char *key);

/* Copy the JSON string value for `key` into out, resolving escapes. Needed for
 * real: the model's answer arrives as an escaped JSON document nested inside a
 * JSON string. \u escapes outside printable Latin (0x20–0x7e) are dropped —
 * species names are Latin-script, so this avoids dragging in a UTF-8 encoder. */
bool  cu_json_str(const char *j, const char *key, char *out, size_t osz);
long  cu_json_num(const char *j, const char *key, long def);
bool  cu_json_bool(const char *j, const char *key, bool def);

/* Neutralise anything that would break the fixed-column CSV visit log or a
 * hand-built JSON reply: commas, quotes, backslashes, newlines -> space. The
 * output schema makes a comma in "Blue Tit, juvenile" unlikely, not impossible. */
void  cu_scrub(char *s);

/* Fill a classify_result_t from the provider's (bird, common, latin, conf)
 * answer using classify.cpp's decide contract: no bird -> species "no bird";
 * bird below the confidence threshold or missing name -> "Unidentified bird"
 * with empty latin (every caller treats empty latin as "do not write a label");
 * otherwise the species + binomial. conf is clamped to 0..100. */
void  cu_to_result(classify_result_t *out, bool bird, const char *common,
                   const char *latin, int conf);

/* Turn a TLS-layer failure into something actionable from the Debug card.
 * esp_http_client collapses every TLS problem into ESP_ERR_HTTP_CONNECT; this
 * reads the mbedTLS code + X.509 flags out of the handle and names the likely
 * cause. Note mbedTLS codes are stored NEGATED (positive here); -0x3000 with
 * certflags 0 is the signature of "no trusted root for this chain", NOT a clean
 * verify. Writes a human-readable string into out. */
void  cu_tls_detail(esp_http_client_handle_t c, char *out, size_t osz);

/* Resolve `host` to a dotted-quad IPv4 in `ip`. Separate from the HTTP client so
 * a verdict can tell "DNS is lying" (a filtering resolver handing back 0.0.0.0)
 * from a TLS/key problem — esp_http_client reports both as HTTP_CONNECT. */
bool  cu_resolve_host(const char *host, char *ip, size_t n);

/* Non-blocking TCP reach test to ip:port, so a verdict can separate "the network
 * won't let this box out to 443" from a TLS/certificate problem. `why` gets the
 * errno text on failure. */
bool  cu_tcp_probe(const char *ip, int port, int timeout_ms, char *why, size_t wsz);

/* Encoded length of base64 for n input bytes: 4*ceil(n/3). Exact, so the caller
 * can set Content-Length up front and stream the body instead of buffering it. */
size_t cu_b64_len(size_t n);

/* Write the whole buffer to an opened esp_http_client, looping over short
 * writes. false on any transport error. */
bool  cu_write(esp_http_client_handle_t c, const char *b, size_t n);

/* Base64-encode `data` straight to the client in 3 KB input chunks (each a
 * multiple of 3, so only the final chunk pads and the stream is one valid
 * base64 document). A call costs ~4 KB of scratch, not a second copy of the
 * image. false on encode/transport error. */
bool  cu_stream_b64(esp_http_client_handle_t c, const uint8_t *data, size_t len);

/* True for HTTP statuses worth retrying: 429 (rate limited) and the 5xx a busy
 * provider returns under load (500/502/503/504 — e.g. Gemini's "model is
 * currently experiencing high demand" 503). A 4xx like 400/401/404 (bad request,
 * bad key, missing model) is deterministic and must NOT be retried. */
bool  cu_status_transient(int status);

/* Backoff before a retry: sleeps ~0.8 s × (attempt+1) so a transient provider
 * spike gets a moment to clear. Called from the classifier / identify-worker
 * task, never from httpd or motion. */
void  cu_retry_backoff(int attempt);

#ifdef __cplusplus
}
#endif
