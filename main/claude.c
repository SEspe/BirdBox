/* Cloud species ID via the Anthropic Claude API (FSD §3.2.3) — see claude.h
 * for the contract and why this exists.
 *
 * Three design points worth knowing before editing:
 *
 * 1. The request body is STREAMED, not assembled. The Messages API wants the
 *    image base64'd inline in the JSON, which is 1.33x the file size on top of
 *    the JPEG itself — with ~600-700 KB of free heap in practice (§7),
 *    buffering the whole body would roughly triple the cost of a call for no
 *    reason. Base64's encoded length is exactly computable (4*ceil(n/3)), so we
 *    can set Content-Length up front and encode straight out of the caller's
 *    buffer in 3 KB chunks. A call therefore costs one JPEG-sized allocation
 *    (the caller's) plus ~4 KB, not three JPEGs.
 *
 * 2. Everything the model returns is untrusted free text. Names are scrubbed of
 *    CSV/JSON metacharacters at the boundary (see scrub()) — the visit log is
 *    fixed-column CSV and the API replies are hand-assembled strings, so a
 *    comma in "Blue Tit, juvenile" would silently shift columns for every later
 *    field. The output schema makes that unlikely, not impossible.
 *
 * 3. Opus 4.8 REMOVED the sampling parameters. Sending temperature, top_p,
 *    top_k or thinking.budget_tokens is a hard 400, so this body must stay
 *    minimal: model, max_tokens, system, output_config, messages. Determinism
 *    comes from the JSON schema, not from temperature 0. Omitting `thinking`
 *    entirely means the model answers without extended thinking, which is what
 *    a single-image visual ID wants — thinking would add latency and cost per
 *    bird event for no accuracy gain. */
#include "claude.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

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
#define B64_CHUNK_IN      3072           /* multiple of 3 => only the final
                                            chunk pads, so the stream is a
                                            valid single base64 document */
#define B64_CHUNK_OUT     (B64_CHUNK_IN / 3 * 4 + 4)

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

bool claude_enabled(void)
{
    return g_settings.claude_enabled && claude_have_key();
}

const char *claude_last_error(void)       { return s_last_error; }
int32_t     claude_last_duration_ms(void) { return s_last_ms; }
uint32_t    claude_call_count(void)       { return s_calls; }

/* ── Minimal JSON reader ────────────────────────────────────────────────────
 * The project has no cJSON (every other reply here is hand-assembled) and
 * pulling it in for four fields would cost more heap than it saves. These find
 * a key at any depth, which is fine for the shapes we read: the reply has
 * exactly one text block, and the schema'd payload inside it has exactly one
 * of each field. */

/* Returns a pointer to the value after "key": , or NULL.
 *
 * Keeps scanning past a match that is NOT followed by a colon, because such a
 * match is a *value*, not a key. This is load-bearing for this API, not a
 * hypothetical: a content block is {"type":"text","text":"..."} , so the first
 * occurrence of "text" in the reply is the value of "type" and is followed by
 * a comma. Stopping at the first hit would make every successful reply look
 * unparseable. */
static const char *json_seek(const char *j, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);

    for (const char *p = strstr(j, pat); p; p = strstr(p + plen, pat)) {
        const char *q = p + plen;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') continue;               /* a value, keep looking */
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        return q;
    }
    return NULL;
}

/* Copies the JSON string starting at `p` (the opening quote) into out,
 * resolving escapes. Needed for real: the model's answer arrives as an
 * escaped JSON document nested inside a JSON string. */
static bool json_str_at(const char *p, char *out, size_t osz)
{
    if (!p || *p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (!*p) return false;
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                if (!p[0] || !p[1] || !p[2] || !p[3]) return false;
                char hex[5] = { p[0], p[1], p[2], p[3], '\0' };
                long v = strtol(hex, NULL, 16);
                p += 4;
                if (v < 0x20 || v > 0x7e) continue;   /* species names are
                                                       * Latin-script; drop
                                                       * anything needing a
                                                       * UTF-8 encoder */
                c = (char) v;
                break;
            }
            default: c = e; break;                    /* \" \\ \/ */
            }
        }
        if (o + 1 < osz) out[o++] = c;
    }
    if (*p != '"') return false;
    out[o] = '\0';
    return true;
}

static bool json_get_str(const char *j, const char *key, char *out, size_t osz)
{
    return json_str_at(json_seek(j, key), out, osz);
}

static long json_get_num(const char *j, const char *key, long def)
{
    const char *p = json_seek(j, key);
    if (!p) return def;
    char *end;
    long v = strtol(p, &end, 10);
    return end == p ? def : v;
}

static bool json_get_bool(const char *j, const char *key, bool def)
{
    const char *p = json_seek(j, key);
    if (!p) return def;
    if (strncmp(p, "true",  4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

/* Neutralise anything that would break the CSV visit log or a hand-built JSON
 * reply. See the file header — this is a real hazard, not defensive noise. */
static void scrub(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\\' || *p == '\n' || *p == '\r')
            *p = ' ';
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = '\0';
}

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

/* Reuses the existing region_filter setting (§3.2.1) rather than adding a
 * second, divergent notion of "where is this camera".
 *
 * additionalProperties:false and a complete `required` list are mandatory for
 * structured outputs — without them the request is rejected. With them the
 * reply is guaranteed to parse, which is what lets the reader below be four
 * key lookups instead of a JSON parser. */
static void build_prefix(char *out, size_t osz)
{
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
        g_settings.region_filter
            ? "The camera is in Northern Europe (the Nordic region) - only "
              "identify species that occur there. "
            : "");
}

static const char REQ_SUFFIX[] =
    "\"}},"
    "{\"type\":\"text\",\"text\":\"Identify the bird in this photo.\"}"
    "]}]}";

static bool wr(esp_http_client_handle_t c, const char *b, size_t n)
{
    while (n) {
        int w = esp_http_client_write(c, b, n);
        if (w <= 0) return false;
        b += w;
        n -= w;
    }
    return true;
}

/* Same species-decision contract as classify.cpp's decide step: an
 * inconclusive answer yields an empty latin, which every caller treats as
 * "do not write a label". */
static void to_result(classify_result_t *out, bool bird, const char *common,
                      const char *latin, int conf)
{
    memset(out, 0, sizeof(*out));
    if (conf < 0)   conf = 0;
    if (conf > 100) conf = 100;
    out->confidence_pct = (uint8_t) conf;
    out->top_pct[0]     = (uint8_t) conf;

    if (bird && latin[0] && common[0])
        snprintf(out->top_label[0], sizeof(out->top_label[0]), "%s (%s)", latin, common);
    else
        strlcpy(out->top_label[0], bird ? "unidentified bird" : "background",
                sizeof(out->top_label[0]));

    if (!bird) {
        strlcpy(out->species, "no bird", sizeof(out->species));
    } else if (!common[0] || !latin[0] || conf < g_settings.confidence_pct) {
        strlcpy(out->species, "Unidentified bird", sizeof(out->species));
    } else {
        strlcpy(out->species, common, sizeof(out->species));
        strlcpy(out->latin,   latin,  sizeof(out->latin));
    }
}

/* Turn a TLS-layer failure into something a person can act on from the Debug
 * card. esp_http_client collapses every TLS problem into ESP_ERR_HTTP_CONNECT,
 * which on its own cannot distinguish "no internet" from "untrusted cert".
 *
 * Note the sign convention: esp-tls stores the mbedTLS code NEGATED (positive
 * here), so 0x3000 means mbedTLS returned -0x3000. Print it raw rather than
 * re-negating it — a previous version of this printed (unsigned)-tls_err and
 * produced 0xFFFFD000, which reads like garbage and cost real debugging time.
 *
 * certflags is the X.509 verify bitmask, and 0 does NOT mean "the certificate
 * was fine": esp_crt_bundle's verify callback returns -0x3000
 * (MBEDTLS_ERR_X509_FATAL_ERROR) WITHOUT setting any flag when it cannot find
 * a trusted root for the chain the server sent. -0x3000 + certflags 0 is
 * therefore the signature of a trust-store failure, not a clean chain. */
static void tls_detail(esp_http_client_handle_t c, char *out, size_t osz)
{
    int tls_err = 0, tls_flags = 0;
    esp_err_t stage = esp_http_client_get_and_clear_last_tls_error(c, &tls_err, &tls_flags);

    const char *hint = "";
    if (tls_err == 0x3000)
        hint = " - no trusted root for this chain in the CA bundle";
    else if (tls_flags & 0x08)          /* MBEDTLS_X509_BADCERT_NOT_TRUSTED */
        hint = " - chain not trusted";
    else if (tls_flags & 0x02)          /* MBEDTLS_X509_BADCERT_EXPIRED   */
        hint = " - certificate expired (device clock wrong?)";
    else if (tls_flags & 0x200)         /* MBEDTLS_X509_BADCERT_FUTURE    */
        hint = " - certificate not yet valid (device clock pre-SNTP?)";

    snprintf(out, osz, "mbedtls -0x%04X, certflags 0x%08X, stage %s%s",
             (unsigned) tls_err, (unsigned) tls_flags, esp_err_to_name(stage), hint);
}

esp_err_t claude_classify_jpeg(const uint8_t *jpeg, size_t len,
                               classify_result_t *out)
{
    /* Key only — the toggle governs the live path, not this. The Gallery's
     * manual button must work with cloud identification switched off. */
    if (!claude_have_key()) {
        fail("no Claude API key");
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg || len == 0 || len > CLAUDE_MAX_JPEG) {
        fail("bad or oversized JPEG (%u B)", (unsigned) len);
        return ESP_ERR_INVALID_ARG;
    }

    char prefix[1280];
    build_prefix(prefix, sizeof(prefix));

    size_t b64_len = 4 * ((len + 2) / 3);
    size_t total   = strlen(prefix) + b64_len + strlen(REQ_SUFFIX);

    uint8_t *chunk = heap_caps_malloc(B64_CHUNK_OUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char    *resp  = heap_caps_malloc(CLAUDE_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk || !resp) {
        free(chunk);
        free(resp);
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
        free(chunk);
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
        tls_detail(c, d, sizeof(d));
        fail("cannot reach Claude (%s; %s)", esp_err_to_name(err), d);
        ret = (err == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        goto done;
    }

    if (!wr(c, prefix, strlen(prefix))) { fail("upload failed (header)"); goto done; }
    for (size_t off = 0; off < len; off += B64_CHUNK_IN) {
        size_t n  = (len - off < B64_CHUNK_IN) ? len - off : B64_CHUNK_IN;
        size_t ol = 0;
        if (mbedtls_base64_encode(chunk, B64_CHUNK_OUT, &ol, jpeg + off, n) != 0) {
            fail("base64 encode failed");
            goto done;
        }
        if (!wr(c, (const char *) chunk, ol)) { fail("upload failed (image)"); goto done; }
    }
    if (!wr(c, REQ_SUFFIX, strlen(REQ_SUFFIX))) { fail("upload failed (prompt)"); goto done; }

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
        json_get_str(resp, "message", msg, sizeof(msg));
        fail("Claude HTTP %d%s%s", status, msg[0] ? ": " : "", msg);
        goto done;
    }

    /* Structured outputs guarantee the assistant text IS the schema'd JSON, so
     * unwrap one layer (content[].text is an escaped JSON document) and read
     * the fields out of it. */
    char text[256];
    if (!json_get_str(resp, "text", text, sizeof(text))) {
        /* A refusal or a max_tokens truncation lands here (no text block). */
        char reason[48] = "";
        json_get_str(resp, "stop_reason", reason, sizeof(reason));
        fail("unparseable reply%s%s", reason[0] ? ", stop_reason " : "", reason);
        goto done;
    }

    char common[64] = "", latin[64] = "";
    json_get_str(text, "common", common, sizeof(common));
    json_get_str(text, "latin",  latin,  sizeof(latin));
    scrub(common);
    scrub(latin);
    bool bird = json_get_bool(text, "bird", false);
    int  conf = (int) json_get_num(text, "confidence", 0);

    to_result(out, bird, common, latin, conf);
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
    free(chunk);
    free(resp);
    if (ret != ESP_OK) s_last_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    return ret;
}

/* ── Connection test ────────────────────────────────────────────────────── */

/* Resolve separately from the HTTP client so a verdict can distinguish "DNS is
 * lying to us" (a filtering resolver returning 0.0.0.0 for the host is a real
 * failure mode on home networks) from "TLS/key problem" — esp_http_client
 * collapses both into ESP_ERR_HTTP_CONNECT. */
static bool resolve_api_host(char *ip, size_t n)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(CLAUDE_API_HOST, "443", &hints, &res) != 0 || !res) return false;
    struct sockaddr_in *a = (struct sockaddr_in *) res->ai_addr;
    inet_ntop(AF_INET, &a->sin_addr, ip, n);
    freeaddrinfo(res);
    return true;
}

/* Plain TCP reach test, so a verdict can separate "the network won't let this
 * box out to 443" from "TLS/certificate problem" — esp_http_client reports both
 * as ESP_ERR_HTTP_CONNECT, which is not actionable on its own. Non-blocking +
 * select so a blackholing firewall can't hang the httpd thread for ~75 s. */
static bool tcp_probe(const char *ip, int port, int timeout_ms, char *why, size_t wsz)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { snprintf(why, wsz, "socket() failed"); return false; }

    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, ip, &a.sin_addr);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

    bool ok = false;
    if (connect(s, (struct sockaddr *) &a, sizeof(a)) == 0) {
        ok = true;
    } else if (errno != EINPROGRESS) {
        snprintf(why, wsz, "%s", strerror(errno));
    } else {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        int rc = select(s + 1, NULL, &w, NULL, &tv);
        if (rc == 0) {
            snprintf(why, wsz, "timed out");
        } else if (rc < 0) {
            snprintf(why, wsz, "select: %s", strerror(errno));
        } else {
            int soerr = 0;
            socklen_t l = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &l);
            if (soerr) snprintf(why, wsz, "%s", strerror(soerr));
            else       ok = true;
        }
    }
    close(s);
    return ok;
}

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
        tls_detail(c, d, sizeof(d));
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
    if (!resolve_api_host(ip, sizeof(ip))) {
        snprintf(out, osz, "DNS lookup for %s failed - no internet?", CLAUDE_API_HOST);
        return ESP_FAIL;
    }
    if (strcmp(ip, "0.0.0.0") == 0) {
        snprintf(out, osz, "DNS returned 0.0.0.0 - a filtering resolver is blocking it");
        return ESP_FAIL;
    }

    char why[64] = "";
    if (!tcp_probe(ip, 443, 8000, why, sizeof(why))) {
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
         * and the failure is inside TLS. tls_detail() names the stage; there is
         * deliberately no "retry without verification" fallback here, both
         * because ESP_TLS_INSECURE is off (esp-tls refuses to set up TLS with
         * no CA, so such a probe fails at setup and proves nothing) and because
         * a key-bearing request must never be retried unverified. */
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
        json_get_str(resp, "message", msg, sizeof(msg));
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
