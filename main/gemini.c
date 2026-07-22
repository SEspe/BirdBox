/* Cloud species ID via the Google Gemini API (FSD §3.2.3) — see gemini.h for
 * the contract. The sibling of claude.c: identical job and output contract,
 * different wire format. Shared HTTP/JSON/TLS plumbing is in cloud_util.c; this
 * file holds only what is specific to Gemini's generateContent endpoint.
 *
 * Design points that differ from Claude:
 *
 * 1. Body is still STREAMED (base64 image inline in JSON) for the same heap
 *    reason — see claude.c. The image goes in an inline_data part; the fixed
 *    generationConfig (response schema + prompt) is the suffix after it.
 *
 * 2. The model id is operator-settable (see the GEMINI_MODEL_DEFAULT block). The
 *    body does NOT send thinkingConfig (v2.63): it used to send
 *    thinkingConfig.thinkingBudget=0 to disable thinking, but Google changed
 *    gemini-flash-lite-latest to REJECT that with a bare HTTP 400
 *    "Request contains an invalid argument" (no field detail) — verified live
 *    2026-07-22, removing the field fixed it. Flash-Lite doesn't think by
 *    default anyway, so there's nothing to disable; a single-image ID gains
 *    nothing from reasoning. If a thinking-by-default model is ever selected and
 *    needs it off, re-add thinkingConfig only for that model.
 *
 * 3. Structured output is generationConfig.responseSchema (OpenAPI subset:
 *    UPPERCASE types, no additionalProperties) plus responseMimeType
 *    application/json. The schema'd JSON comes back as the text of the first
 *    candidate part — one escaped JSON document, same unwrap as Claude. */
#include "gemini.h"
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

static const char *TAG = "gemini";

/* The model id is OPERATOR-SETTABLE (settings.gemini_model), because Gemini
 * model availability shifts under you in ways no single pinned id survives —
 * learned the hard way (see FSD v2.08-v2.12):
 *   - gemini-2.5-flash    -> 404 "not available to new users" (account-tier
 *     gated — billing did NOT lift it on this key).
 *   - gemini-flash-latest -> sustained 503 "high demand" on free tier.
 *   - gemini-2.0-flash    -> 404 "no longer available" once the 2.0 generation
 *     started retiring.
 * So rather than chase it in firmware, the id comes from Settings; the Test
 * button lists what a given key can actually call. GEMINI_MODEL_DEFAULT is the
 * fallback when the field is blank: gemini-flash-lite-latest — the rolling
 * lite-flash alias, verified live to return a correct ID in ~4 s. The plain
 * gemini-flash-latest alias resolved too but returned sustained 503 "high
 * demand"; the lite alias is the less-contended (and cheaper) sibling.
 *
 * The body no longer sends thinkingConfig (v2.63): gemini-flash-lite-latest
 * began 400ing "Request contains an invalid argument" on thinkingBudget=0
 * (verified 2026-07-22). Flash-Lite doesn't think by default, so nothing needs
 * disabling. Re-add thinkingConfig only if a thinking-by-default model is ever
 * selected AND that model still accepts it. */
#define GEMINI_API_HOST      "generativelanguage.googleapis.com"
#define GEMINI_MODEL_DEFAULT "gemini-flash-lite-latest"
#define GEMINI_MODELS_URL    "https://" GEMINI_API_HOST "/v1beta/models"

/* Effective model: the operator's choice, or the default when unset. */
static const char *gemini_model(void)
{
    return g_settings.gemini_model[0] ? g_settings.gemini_model : GEMINI_MODEL_DEFAULT;
}

#define GEMINI_MAX_JPEG   (300 * 1024)   /* matches CLASSIFY_MAX_BODY */
#define GEMINI_RESP_MAX   4096           /* schema'd reply is small; slack is for
                                            error bodies + candidate metadata */

static char     s_last_error[256] = "";   /* wide enough for Gemini's full 400 detail (v2.63) */
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

bool gemini_have_key(void)
{
    return g_settings.gemini_key[0] != '\0';
}

const char *gemini_last_error(void)       { return s_last_error; }
int32_t     gemini_last_duration_ms(void) { return s_last_ms; }
uint32_t    gemini_call_count(void)       { return s_calls; }

/* ── Request assembly ───────────────────────────────────────────────────────
 * Body shape, with the base64 streamed in at <IMAGE>:
 *
 *   {"systemInstruction":{"parts":[{"text":".."}]},
 *    "contents":[{"role":"user","parts":[
 *        {"inline_data":{"mime_type":"image/jpeg","data":"<IMAGE>"}},
 *        {"text":"Identify the bird in this photo."}]}],
 *    "generationConfig":{"maxOutputTokens":512,"thinkingConfig":{"thinkingBudget":0},
 *        "responseMimeType":"application/json","responseSchema":{..}}}
 *
 * The prefix is everything up to the image bytes; the suffix (fixed) closes the
 * inline_data part, adds the text prompt and the generationConfig. */

/* Same region constraint as claude.c: when region_filter is on (§3.2.1), Gemini
 * is limited to the operator's curated target list so the cloud tier can't
 * return an off-target species. All entries are ASCII, safe unescaped in JSON. */
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
        "{\"systemInstruction\":{\"parts\":[{\"text\":\""
        "You are an expert ornithologist labelling photos from a garden bird "
        "feeder and nest-box camera. Identify the single most prominent bird "
        "in the image. Set bird to false when no bird is present - an empty "
        "perch, a squirrel or insect, foliage, or a blur too poor to identify. "
        "%s"
        "Give common as the English common name and latin as the scientific "
        "binomial. Set confidence to your certainty from 0 to 100 that the "
        "species is exactly right; be honest and use low values when pose, "
        "blur or framing leaves real doubt between similar species."
        "\"}]},"
        "\"contents\":[{\"role\":\"user\",\"parts\":["
        "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"",
        constraint);
}

static const char REQ_SUFFIX[] =
    "\"}},"
    "{\"text\":\"Identify the bird in this photo.\"}]}],"
    "\"generationConfig\":{\"maxOutputTokens\":512,"
    "\"responseMimeType\":\"application/json\","
    "\"responseSchema\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"bird\":{\"type\":\"BOOLEAN\"},"
    "\"common\":{\"type\":\"STRING\"},"
    "\"latin\":{\"type\":\"STRING\"},"
    "\"confidence\":{\"type\":\"INTEGER\"}},"
    "\"required\":[\"bird\",\"common\",\"latin\",\"confidence\"],"
    "\"propertyOrdering\":[\"bird\",\"common\",\"latin\",\"confidence\"]}}}";

/* One HTTP attempt — see claude.c's claude_post_once for the rationale. Returns
 * ESP_OK with *status set when a response arrived, or a transport error. */
#define GEMINI_MAX_RETRIES 3   /* 4 attempts total; backoff 2s/4s/8s on 429/5xx */

static esp_err_t gemini_post_once(const char *prefix, const uint8_t *jpeg,
                                  size_t len, char *resp, int *status)
{
    size_t total = strlen(prefix) + cu_b64_len(len) + strlen(REQ_SUFFIX);

    /* Model id comes from Settings; esp_http_client_init copies the URL, so this
     * stack buffer is safe to go out of scope after init. */
    char url[176];
    snprintf(url, sizeof(url), "https://%s/v1beta/models/%s:generateContent",
             GEMINI_API_HOST, gemini_model());

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .buffer_size       = 1536,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { fail("http client init failed"); return ESP_FAIL; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "x-goog-api-key", g_settings.gemini_key);

    esp_err_t ret = ESP_FAIL;
    esp_err_t err = esp_http_client_open(c, total);
    if (err != ESP_OK) {
        char d[96];
        cu_tls_detail(c, d, sizeof(d));
        fail("cannot reach Gemini (%s; %s)", esp_err_to_name(err), d);
        ret = (err == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        goto out;
    }
    if (!cu_write(c, prefix, strlen(prefix)))         { fail("upload failed (header)"); goto out; }
    if (!cu_stream_b64(c, jpeg, len))                 { fail("upload failed (image)");  goto out; }
    if (!cu_write(c, REQ_SUFFIX, strlen(REQ_SUFFIX))) { fail("upload failed (prompt)"); goto out; }
    if (esp_http_client_fetch_headers(c) < 0)         { fail("no reply from Gemini");   goto out; }

    int rd = 0, r;
    while (rd < GEMINI_RESP_MAX - 1 &&
           (r = esp_http_client_read(c, resp + rd, GEMINI_RESP_MAX - 1 - rd)) > 0)
        rd += r;
    resp[rd] = '\0';
    *status = esp_http_client_get_status_code(c);
    ret = ESP_OK;

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

esp_err_t gemini_classify_jpeg(const uint8_t *jpeg, size_t len,
                               classify_result_t *out)
{
    if (!gemini_have_key()) {
        fail("no Gemini API key");
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg || len == 0 || len > GEMINI_MAX_JPEG) {
        fail("bad or oversized JPEG (%u B)", (unsigned) len);
        return ESP_ERR_INVALID_ARG;
    }

    char prefix[3400];
    build_prefix(prefix, sizeof(prefix));

    char *resp = heap_caps_malloc(GEMINI_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        fail("out of memory");
        return ESP_ERR_NO_MEM;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; ; attempt++) {
        esp_err_t terr = gemini_post_once(prefix, jpeg, len, resp, &status);
        if (terr != ESP_OK) { ret = terr; goto done; }   /* couldn't reach — no retry */
        if (status == 200) break;
        /* Gemini returns 429 (free-tier 10 RPM rate limit — clears within a
         * minute) and 503 "model is currently experiencing high demand" under
         * load; retry both with exponential backoff (2s/4s/8s, Google's
         * guidance). A 4xx other than 429 (bad key, bad model) is deterministic
         * and is surfaced rather than retried. A genuine daily-quota exhaustion
         * still 429s after all retries and degrades to the on-device model. */
        if (cu_status_transient(status) && attempt < GEMINI_MAX_RETRIES) {
            ESP_LOGW(TAG, "Gemini HTTP %d — retry %d/%d", status, attempt + 1, GEMINI_MAX_RETRIES);
            cu_retry_backoff(attempt);
            continue;
        }
        /* Error bodies are {"error":{"code":..,"message":..,"status":..}} —
         * surface the message verbatim ("API key not valid", "quota exceeded"). */
        char msg[200] = "";
        cu_json_str(resp, "message", msg, sizeof(msg));
        fail("Gemini HTTP %d%s%s", status, msg[0] ? ": " : "", msg);
        goto done;
    }

    /* responseMimeType application/json => the first candidate part's text IS
     * the schema'd JSON document (escaped inside the response JSON). Unwrap one
     * layer, then read the fields out of it. */
    char text[256];
    if (!cu_json_str(resp, "text", text, sizeof(text))) {
        /* No part text: a safety block or a MAX_TOKENS truncation. finishReason
         * names which. */
        char reason[48] = "";
        cu_json_str(resp, "finishReason", reason, sizeof(reason));
        fail("unparseable reply%s%s", reason[0] ? ", finishReason " : "", reason);
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
    free(resp);
    if (ret != ESP_OK) s_last_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    return ret;
}

/* ── Connection test ────────────────────────────────────────────────────── */

/* GET /v1beta/models — free and unmetered, so it validates the key without
 * billing a single token. */
static esp_err_t models_get(int *status, char *resp, char *detail, size_t dsz)
{
    esp_http_client_config_t cfg = {
        .url               = GEMINI_MODELS_URL,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 1536,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(detail, dsz, "http client init failed"); return ESP_FAIL; }
    esp_http_client_set_header(c, "x-goog-api-key", g_settings.gemini_key);

    esp_err_t ret = esp_http_client_open(c, 0);
    if (ret != ESP_OK) {
        char d[96];
        cu_tls_detail(c, d, sizeof(d));
        snprintf(detail, dsz, "%s; %s", esp_err_to_name(ret), d);
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int rd = 0, r;
    while (rd < GEMINI_RESP_MAX - 1 &&
           (r = esp_http_client_read(c, resp + rd, GEMINI_RESP_MAX - 1 - rd)) > 0)
        rd += r;
    resp[rd] = '\0';
    *status = esp_http_client_get_status_code(c);

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

esp_err_t gemini_test(char *out, size_t osz)
{
    if (!gemini_have_key()) {
        snprintf(out, osz, "no API key stored");
        return ESP_ERR_INVALID_STATE;
    }

    char ip[16] = "";
    if (!cu_resolve_host(GEMINI_API_HOST, ip, sizeof(ip))) {
        snprintf(out, osz, "DNS lookup for %s failed - no internet?", GEMINI_API_HOST);
        return ESP_FAIL;
    }
    if (strcmp(ip, "0.0.0.0") == 0) {
        snprintf(out, osz, "DNS returned 0.0.0.0 - a filtering resolver is blocking it");
        return ESP_FAIL;
    }

    char why[64] = "";
    if (!cu_tcp_probe(ip, 443, 8000, why, sizeof(why))) {
        snprintf(out, osz, "%s resolves to %s but TCP 443 is unreachable (%s) - "
                           "outbound HTTPS blocked for this device?", GEMINI_API_HOST, ip, why);
        return ESP_FAIL;
    }

    char *resp = heap_caps_malloc(GEMINI_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) { snprintf(out, osz, "out of memory"); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_FAIL;
    int status = 0;
    char detail[128] = "";
    esp_err_t err = models_get(&status, resp, detail, sizeof(detail));

    if (err != ESP_OK) {
        snprintf(out, osz, "TCP 443 to %s open but TLS failed - %s", ip, detail);
        goto out_done;
    }

    if (status == 200) {
        /* List the flash model ids this key can actually use, so a 404 like
         * "gemini-2.5-flash no longer available to new users" is diagnosable
         * without a serial console — and the operator can see what to override
         * GEMINI_MODEL to if the -latest alias ever mis-resolves. Model names
         * arrive as "name":"models/<id>"; only the first page is returned, so
         * the total is "at least N". */
        char list[220];
        size_t lp = 0;
        int total = 0, flash = 0;
        for (const char *p = resp; (p = strstr(p, "\"models/")) != NULL; p += 8) {
            const char *s = p + 8;
            const char *e = strchr(s, '"');
            if (!e) break;
            total++;
            size_t idl = (size_t) (e - s);
            char id[48];
            if (idl >= sizeof(id)) idl = sizeof(id) - 1;
            memcpy(id, s, idl);
            id[idl] = '\0';
            if (strstr(id, "flash") && flash < 6) {
                lp += snprintf(list + lp, sizeof(list) - lp, "%s%s", flash ? ", " : "", id);
                flash++;
            }
        }
        if (flash)
            snprintf(out, osz, "OK - key valid (%d+ models). Flash: %s", total, list);
        else
            snprintf(out, osz, "OK - key valid (%d+ models reachable at %s)", total, ip);
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

esp_err_t gemini_classify_file(const char *fs_path, classify_result_t *out)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        fail("cannot open %s", fs_path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > GEMINI_MAX_JPEG) {
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
    esp_err_t err = gemini_classify_jpeg(buf, sz, out);
    free(buf);
    return err;
}
