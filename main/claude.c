/* Cloud species ID via the Anthropic Claude API (FSD §3.2.3) — see claude.h
 * for the contract and why this exists. Shared HTTP/JSON/TLS plumbing lives in
 * cloud_util.c; this file holds only what is specific to the Messages API.
 *
 * Three design points worth knowing before editing:
 *
 * 1. The request body is STREAMED, not assembled. The Messages API wants the
 *    image base64'd inline in the JSON, which is 1.33x the file size on top of
 *    the JPEG itself — with ~600-700 KB of free heap in practice (§7),
 *    buffering the whole body would roughly triple the cost of a call for no
 *    reason. Base64's encoded length is exactly computable (4*ceil(n/3)), so we
 *    set Content-Length up front and encode straight out of the caller's buffer
 *    (cu_stream_b64). A call therefore costs one JPEG-sized allocation (the
 *    caller's) plus ~4 KB, not three JPEGs.
 *
 * 2. Everything the model returns is untrusted free text. Names are scrubbed of
 *    CSV/JSON metacharacters at the boundary (cu_scrub) — the visit log is
 *    fixed-column CSV and the API replies are hand-assembled strings, so a comma
 *    in "Blue Tit, juvenile" would silently shift columns for every later field.
 *
 * 3. Opus 4.8 REMOVED the sampling parameters. Sending temperature, top_p,
 *    top_k or thinking.budget_tokens is a hard 400, so this body must stay
 *    minimal: model, max_tokens, system, output_config, messages. Determinism
 *    comes from the JSON schema, not from temperature 0. Omitting `thinking`
 *    entirely means the model answers without extended thinking, which is what a
 *    single-image visual ID wants — thinking would add latency and cost per bird
 *    event for no accuracy gain. */
#include "claude.h"
#include "cloud_util.h"
#include "settings.h"
#include "target_species.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "claude";

/* Opus 4.8 is the project default and the strongest vision model available; a
 * misidentified bird costs more here than the tokens do, because a wrong label
 * confirmed by a tired human poisons the §3.2.2 training set. Model IDs are
 * complete as-is — never append a date suffix. */
#define CLAUDE_API_HOST  "api.anthropic.com"
#define CLAUDE_URL       "https://" CLAUDE_API_HOST "/v1/messages"
#define CLAUDE_MODELS_URL "https://" CLAUDE_API_HOST "/v1/models"
#define CLAUDE_MODEL     "claude-opus-4-8"
#define CLAUDE_API_VER   "2023-06-01"

#define CLAUDE_MAX_JPEG   (300 * 1024)   /* matches CLASSIFY_MAX_BODY */
#define CLAUDE_RESP_MAX   4096           /* schema'd reply is ~120 B; the slack
                                            is for API error bodies */

static char     s_last_error[96] = "";
static int32_t  s_last_ms       = -1;
static uint32_t s_calls         = 0;

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", s_last_error);
}

bool claude_have_key(void)
{
    return g_settings.claude_key[0] != '\0';
}

const char *claude_last_error(void)       { return s_last_error; }
int32_t     claude_last_duration_ms(void) { return s_last_ms; }
uint32_t    claude_call_count(void)       { return s_calls; }

/* ── Request assembly ───────────────────────────────────────────────────────
 * Body shape, with the base64 streamed in at <IMAGE>:
 *
 *   {"model":..,"max_tokens":..,"system":"..","output_config":{"format":{..}},
 *    "messages":[{"role":"user","content":[
 *        {"type":"image","source":{"type":"base64","media_type":"image/jpeg",
 *                                  "data":"<IMAGE>"}},
 *        {"type":"text","text":"..."}]}]}
 *
 * The image block goes BEFORE the text block: the API documents better results
 * when the question follows the image. */

/* When region_filter is on (§3.2.1), Claude is constrained to the operator's
 * curated target list (target_species.c — the same 30 the relabel picker and
 * the iNat mask use), so the cloud tier can't return a geographically absurd or
 * off-target species. All entries are ASCII (letters/space/paren/hyphen), so
 * the list is safe inside the JSON string with no escaping.
 *
 * additionalProperties:false and a complete `required` list are mandatory for
 * structured outputs — without them the request is rejected. With them the
 * reply is guaranteed to parse, which is what lets the reader below be four key
 * lookups instead of a JSON parser. */
static void build_prefix(char *out, size_t osz)
{
    char constraint[1600];
    constraint[0] = '\0';
    if (g_settings.region_filter) {
        char list[1300];
        size_t lp = 0;
        for (size_t i = 0; i < TARGET_SPECIES_N && lp < sizeof(list); i++)
            lp += snprintf(list + lp, sizeof(list) - lp, "%s%s (%s)",
                           i ? ", " : "",
                           TARGET_SPECIES[i].common, TARGET_SPECIES[i].latin);
        snprintf(constraint, sizeof(constraint),
            "The camera is in Northern Europe (the Nordic region). Choose the "
            "species only from this list (common name - scientific name): %s. "
            "If the bird clearly matches none of them, give your closest pick "
            "but set a low confidence. ", list);
    }

    snprintf(out, osz,
        "{\"model\":\"" CLAUDE_MODEL "\",\"max_tokens\":512,"
        "\"system\":\""
        "You are an expert ornithologist labelling photos from a garden bird "
        "feeder and nest-box camera. Identify the single most prominent bird "
        "in the image. Set bird to false when no bird is present - an empty "
        "perch, a squirrel or insect, foliage, or a blur too poor to identify. "
        "%s"
        "Give common as the English common name and latin as the scientific "
        "binomial. Set confidence to your certainty from 0 to 100 that the "
        "species is exactly right; be honest and use low values when pose, "
        "blur or framing leaves real doubt between similar species."
        "\","
        "\"output_config\":{\"format\":{\"type\":\"json_schema\",\"schema\":{"
        "\"type\":\"object\",\"properties\":{"
        "\"bird\":{\"type\":\"boolean\"},"
        "\"common\":{\"type\":\"string\"},"
        "\"latin\":{\"type\":\"string\"},"
        "\"confidence\":{\"type\":\"integer\"}},"
        "\"required\":[\"bird\",\"common\",\"latin\",\"confidence\"],"
        "\"additionalProperties\":false}}},"
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
        "\"media_type\":\"image/jpeg\",\"data\":\"",
        constraint);
}

static const char REQ_SUFFIX[] =
    "\"}},"
    "{\"type\":\"text\",\"text\":\"Identify the bird in this photo.\"}"
    "]}]}";

esp_err_t claude_classify_jpeg(const uint8_t *jpeg, size_t len,
                               classify_result_t *out)
{
    /* Key only — the toggle governs the live path (cloud.c), not this. The
     * Gallery's manual button must work with cloud identification switched off. */
    if (!claude_have_key()) {
        fail("no Claude API key");
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg || len == 0 || len > CLAUDE_MAX_JPEG) {
        fail("bad or oversized JPEG (%u B)", (unsigned) len);
        return ESP_ERR_INVALID_ARG;
    }

    char prefix[3200];
    build_prefix(prefix, sizeof(prefix));

    size_t total = strlen(prefix) + cu_b64_len(len) + strlen(REQ_SUFFIX);

    char *resp = heap_caps_malloc(CLAUDE_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        fail("out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url               = CLAUDE_URL,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .buffer_size       = 1536,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(resp);
        fail("http client init failed");
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "x-api-key", g_settings.claude_key);
    esp_http_client_set_header(c, "anthropic-version", CLAUDE_API_VER);

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, total);
    if (err != ESP_OK) {
        char d[96];
        cu_tls_detail(c, d, sizeof(d));
        fail("cannot reach Claude (%s; %s)", esp_err_to_name(err), d);
        ret = (err == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        goto done;
    }

    if (!cu_write(c, prefix, strlen(prefix)))    { fail("upload failed (header)"); goto done; }
    if (!cu_stream_b64(c, jpeg, len))            { fail("upload failed (image)");  goto done; }
    if (!cu_write(c, REQ_SUFFIX, strlen(REQ_SUFFIX))) { fail("upload failed (prompt)"); goto done; }

    if (esp_http_client_fetch_headers(c) < 0) { fail("no reply from Claude"); goto done; }

    int rd = 0, r;
    while (rd < CLAUDE_RESP_MAX - 1 &&
           (r = esp_http_client_read(c, resp + rd, CLAUDE_RESP_MAX - 1 - rd)) > 0)
        rd += r;
    resp[rd] = '\0';

    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /* Error bodies are {"type":"error","error":{"type":..,"message":..}} —
         * surface the message verbatim to the Debug card; it is the difference
         * between "invalid x-api-key" and "credit balance too low", and there
         * is no serial cable in the field. */
        char msg[80] = "";
        cu_json_str(resp, "message", msg, sizeof(msg));
        fail("Claude HTTP %d%s%s", status, msg[0] ? ": " : "", msg);
        goto done;
    }

    /* Structured outputs guarantee the assistant text IS the schema'd JSON, so
     * unwrap one layer (content[].text is an escaped JSON document) and read the
     * fields out of it. */
    char text[256];
    if (!cu_json_str(resp, "text", text, sizeof(text))) {
        /* A refusal or a max_tokens truncation lands here (no text block). */
        char reason[48] = "";
        cu_json_str(resp, "stop_reason", reason, sizeof(reason));
        fail("unparseable reply%s%s", reason[0] ? ", stop_reason " : "", reason);
        goto done;
    }

    char common[64] = "", latin[64] = "";
    cu_json_str(text, "common", common, sizeof(common));
    cu_json_str(text, "latin",  latin,  sizeof(latin));
    cu_scrub(common);
    cu_scrub(latin);
    bool bird = cu_json_bool(text, "bird", false);
    int  conf = (int) cu_json_num(text, "confidence", 0);

    cu_to_result(out, bird, common, latin, conf);
    out->duration_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    s_last_ms = out->duration_ms;
    s_last_error[0] = '\0';
    s_calls++;
    ESP_LOGI(TAG, "%s (%u%%, %ld ms, %u KB image)", out->species,
             out->confidence_pct, (long) out->duration_ms, (unsigned) (len / 1024));
    ret = ESP_OK;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(resp);
    if (ret != ESP_OK) s_last_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    return ret;
}

/* ── Connection test ────────────────────────────────────────────────────── */

/* GET /v1/models — free and unmetered, so it validates the key without billing
 * a single token. */
static esp_err_t models_get(int *status, char *resp, char *detail, size_t dsz)
{
    esp_http_client_config_t cfg = {
        .url               = CLAUDE_MODELS_URL,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 1536,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(detail, dsz, "http client init failed"); return ESP_FAIL; }
    esp_http_client_set_header(c, "x-api-key", g_settings.claude_key);
    esp_http_client_set_header(c, "anthropic-version", CLAUDE_API_VER);

    esp_err_t ret = esp_http_client_open(c, 0);
    if (ret != ESP_OK) {
        char d[96];
        cu_tls_detail(c, d, sizeof(d));
        snprintf(detail, dsz, "%s; %s", esp_err_to_name(ret), d);
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int rd = 0, r;
    while (rd < CLAUDE_RESP_MAX - 1 &&
           (r = esp_http_client_read(c, resp + rd, CLAUDE_RESP_MAX - 1 - rd)) > 0)
        rd += r;
    resp[rd] = '\0';
    *status = esp_http_client_get_status_code(c);

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

esp_err_t claude_test(char *out, size_t osz)
{
    if (!claude_have_key()) {
        snprintf(out, osz, "no API key stored");
        return ESP_ERR_INVALID_STATE;
    }

    char ip[16] = "";
    if (!cu_resolve_host(CLAUDE_API_HOST, ip, sizeof(ip))) {
        snprintf(out, osz, "DNS lookup for %s failed - no internet?", CLAUDE_API_HOST);
        return ESP_FAIL;
    }
    if (strcmp(ip, "0.0.0.0") == 0) {
        snprintf(out, osz, "DNS returned 0.0.0.0 - a filtering resolver is blocking it");
        return ESP_FAIL;
    }

    char why[64] = "";
    if (!cu_tcp_probe(ip, 443, 8000, why, sizeof(why))) {
        snprintf(out, osz, "%s resolves to %s but TCP 443 is unreachable (%s) - "
                           "outbound HTTPS blocked for this device?", CLAUDE_API_HOST, ip, why);
        return ESP_FAIL;
    }

    char *resp = heap_caps_malloc(CLAUDE_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) { snprintf(out, osz, "out of memory"); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_FAIL;
    int status = 0;
    char detail[128] = "";
    esp_err_t err = models_get(&status, resp, detail, sizeof(detail));

    if (err != ESP_OK) {
        /* DNS resolved and raw TCP 443 answered, so the path to the API is open
         * and the failure is inside TLS. cu_tls_detail() names the stage; there
         * is deliberately no "retry without verification" fallback here, both
         * because ESP_TLS_INSECURE is off (esp-tls refuses to set up TLS with no
         * CA, so such a probe fails at setup and proves nothing) and because a
         * key-bearing request must never be retried unverified. */
        snprintf(out, osz, "TCP 443 to %s open but TLS failed - %s", ip, detail);
        goto out_done;
    }

    if (status == 200) {
        int models = 0;
        for (const char *p = resp; (p = strstr(p, "\"display_name\"")) != NULL; p += 14) models++;
        snprintf(out, osz, "OK - key valid, %d model%s reachable at %s",
                 models, models == 1 ? "" : "s", ip);
        ret = ESP_OK;
    } else {
        char msg[80] = "";
        cu_json_str(resp, "message", msg, sizeof(msg));
        snprintf(out, osz, "reached %s but the key was rejected: HTTP %d%s%s",
                 ip, status, msg[0] ? " - " : "", msg);
    }

out_done:
    free(resp);
    snprintf(s_last_error, sizeof(s_last_error), "%s", ret == ESP_OK ? "" : out);
    return ret;
}

esp_err_t claude_classify_file(const char *fs_path, classify_result_t *out)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        fail("cannot open %s", fs_path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > CLAUDE_MAX_JPEG) {
        fclose(f);
        fail("bad or oversized JPEG (%ld B)", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        fail("out of memory");
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if (got != (size_t) sz) {
        free(buf);
        fail("read failed");
        return ESP_FAIL;
    }
    esp_err_t err = claude_classify_jpeg(buf, sz, out);
    free(buf);
    return err;
}
