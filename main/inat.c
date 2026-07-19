/* Cloud species ID via the iNaturalist Computer Vision API (FSD §3.2.3) — see
 * inat.h for the contract and the auth caveat (24 h JWT). Shared HTTP/JSON/TLS
 * plumbing is in cloud_util.c; this file holds the score_image specifics.
 *
 * Wire format differs from the Claude/Gemini tiers: score_image wants a
 * **multipart/form-data** file upload (the raw JPEG in an `image` part), not a
 * base64-in-JSON body. So the image is streamed RAW (no base64) between a fixed
 * preamble and trailer, and Content-Length is preamble+jpeg+trailer. */
#include "inat.h"
#include "cloud_util.h"
#include "settings.h"
#include "version.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "inat";

#define INAT_API_HOST  "api.inaturalist.org"
#define INAT_CV_URL    "https://" INAT_API_HOST "/v1/computervision/score_image"
#define INAT_ME_URL    "https://" INAT_API_HOST "/v1/users/me"
#define INAT_UA        "BirdBox/" FIRMWARE_VERSION " (ESP32 nest-box camera)"
#define INAT_BOUNDARY  "----BirdBoxCVb0undaryX9f2"

#define INAT_MAX_JPEG  (300 * 1024)
#define INAT_RESP_MAX  16384    /* CV replies are large (taxon + ancestors); the
                                   top result (all we read) is well inside this */

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

bool inat_have_token(void) { return g_settings.inat_key[0] != '\0'; }
bool inat_cv_enabled(void) { return g_settings.inat_cv_enabled && inat_have_token(); }

const char *inat_last_error(void)       { return s_last_error; }
int32_t     inat_last_duration_ms(void) { return s_last_ms; }
uint32_t    inat_call_count(void)       { return s_calls; }

/* Multipart parts around the raw JPEG. When a geo hint is set, lat+lng parts are
 * prepended (see build_preamble) — iNat's CV is far stronger with location. */
#define MP_IMG \
    "--" INAT_BOUNDARY "\r\n" \
    "Content-Disposition: form-data; name=\"image\"; filename=\"b.jpg\"\r\n" \
    "Content-Type: image/jpeg\r\n\r\n"
static const char MP_POST[] = "\r\n--" INAT_BOUNDARY "--\r\n";

/* Build the multipart preamble (everything up to the raw JPEG bytes): optional
 * lat/lng form parts from settings.inat_loc ("lat,lng", already validated to
 * [0-9.,-]), then the image part header. */
static void build_preamble(char *out, size_t osz)
{
    int n = 0;
    const char *comma = g_settings.inat_loc[0] ? strchr(g_settings.inat_loc, ',') : NULL;
    if (comma) {
        char lat[16] = "", lng[16] = "";
        size_t la = (size_t) (comma - g_settings.inat_loc);
        if (la < sizeof(lat)) { memcpy(lat, g_settings.inat_loc, la); lat[la] = '\0'; }
        strlcpy(lng, comma + 1, sizeof(lng));
        n += snprintf(out + n, osz - n,
            "--" INAT_BOUNDARY "\r\nContent-Disposition: form-data; name=\"lat\"\r\n\r\n%s\r\n"
            "--" INAT_BOUNDARY "\r\nContent-Disposition: form-data; name=\"lng\"\r\n\r\n%s\r\n",
            lat, lng);
    }
    snprintf(out + n, osz - n, "%s", MP_IMG);
}

/* Auth header value "Bearer <jwt>" — the JWT is ~300-800 chars. */
static void bearer(char *out, size_t osz)
{
    snprintf(out, osz, "Bearer %s", g_settings.inat_key);
}

esp_err_t inat_classify_jpeg(const uint8_t *jpeg, size_t len, classify_result_t *out)
{
    if (!inat_have_token()) { fail("no iNaturalist token"); return ESP_ERR_INVALID_STATE; }
    if (!jpeg || len == 0 || len > INAT_MAX_JPEG) {
        fail("bad or oversized JPEG (%u B)", (unsigned) len);
        return ESP_ERR_INVALID_ARG;
    }

    char *resp = heap_caps_malloc(INAT_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *auth = heap_caps_malloc(900, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp || !auth) { free(resp); free(auth); fail("out of memory"); return ESP_ERR_NO_MEM; }
    bearer(auth, 900);

    char pre[400];
    build_preamble(pre, sizeof(pre));
    size_t total = strlen(pre) + len + strlen(MP_POST);

    /* Geo is passed as URL QUERY params (lat/lng) — that's what score_image's
     * geomodel reads; the multipart lat/lng parts are kept as belt-and-suspenders.
     * inat_loc is validated [0-9.,-] so it's URL-safe. */
    char url[128];
    if (g_settings.inat_loc[0] && strchr(g_settings.inat_loc, ',')) {
        char lat[16] = "", lng[16] = "";
        const char *comma = strchr(g_settings.inat_loc, ',');
        size_t la = (size_t) (comma - g_settings.inat_loc);
        if (la < sizeof(lat)) { memcpy(lat, g_settings.inat_loc, la); lat[la] = '\0'; }
        strlcpy(lng, comma + 1, sizeof(lng));
        snprintf(url, sizeof(url), "%s?lat=%s&lng=%s", INAT_CV_URL, lat, lng);
    } else {
        strlcpy(url, INAT_CV_URL, sizeof(url));
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(resp); free(auth); fail("http client init failed"); return ESP_FAIL; }
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" INAT_BOUNDARY);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "User-Agent", INAT_UA);
    esp_http_client_set_header(c, "Accept", "application/json");

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, total);
    if (err != ESP_OK) {
        char d[96];
        cu_tls_detail(c, d, sizeof(d));
        fail("cannot reach iNaturalist (%s; %s)", esp_err_to_name(err), d);
        ret = (err == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        goto done;
    }
    if (!cu_write(c, pre, strlen(pre)))          { fail("upload failed (header)"); goto done; }
    if (!cu_write(c, (const char *) jpeg, len))  { fail("upload failed (image)");  goto done; }
    if (!cu_write(c, MP_POST, strlen(MP_POST)))  { fail("upload failed (trailer)"); goto done; }

    if (esp_http_client_fetch_headers(c) < 0) { fail("no reply from iNaturalist"); goto done; }
    int rd = 0, r;
    while (rd < INAT_RESP_MAX - 1 &&
           (r = esp_http_client_read(c, resp + rd, INAT_RESP_MAX - 1 - rd)) > 0)
        rd += r;
    resp[rd] = '\0';

    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /* 401 = token missing/expired (the 24 h JWT); error body is
         * {"error":"...","status":401}. Surface it for the Debug card. */
        char msg[80] = "";
        cu_json_str(resp, "error", msg, sizeof(msg));
        fail("iNat HTTP %d%s%s", status, msg[0] ? ": " : "", msg);
        goto done;
    }

    /* Top result: results[0].combined_score + results[0].taxon.{name,
     * preferred_common_name, rank, iconic_taxon_name}. cu_json_* find the FIRST
     * occurrence, which is results[0] (first in the array). Only a bird
     * (iconic Aves) at species rank counts as a confident ID; anything else
     * yields empty latin so classify.cpp falls through to the nordic tier. */
    char latin[64] = "", common[64] = "", rank[24] = "", iconic[16] = "";
    cu_json_str(resp, "name", latin, sizeof(latin));
    cu_json_str(resp, "preferred_common_name", common, sizeof(common));
    cu_json_str(resp, "rank", rank, sizeof(rank));
    cu_json_str(resp, "iconic_taxon_name", iconic, sizeof(iconic));
    int score = (int) cu_json_num(resp, "combined_score", 0);
    cu_scrub(latin);
    cu_scrub(common);

    bool bird_species = strcmp(rank, "species") == 0 && strcmp(iconic, "Aves") == 0 &&
                        latin[0] && common[0];
    if (bird_species)
        cu_to_result(out, true, common, latin, score);   /* threshold applied inside */
    else
        cu_to_result(out, true, "", "", score);           /* -> "Unidentified bird" */

    out->duration_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    s_last_ms = out->duration_ms;
    s_last_error[0] = '\0';
    s_calls++;
    ESP_LOGI(TAG, "%s (%u%%, %ld ms, %u KB)", out->species, out->confidence_pct,
             (long) out->duration_ms, (unsigned) (len / 1024));
    ret = ESP_OK;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(resp);
    free(auth);
    if (ret != ESP_OK) s_last_ms = (int32_t) ((esp_timer_get_time() - t0) / 1000);
    return ret;
}

esp_err_t inat_classify_file(const char *fs_path, classify_result_t *out)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) { fail("cannot open %s", fs_path); return ESP_ERR_NOT_FOUND; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > INAT_MAX_JPEG) {
        fclose(f);
        fail("bad or oversized JPEG (%ld B)", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); fail("out of memory"); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if (got != (size_t) sz) { free(buf); fail("read failed"); return ESP_FAIL; }
    esp_err_t err = inat_classify_jpeg(buf, sz, out);
    free(buf);
    return err;
}

/* ── Connection / token test ─────────────────────────────────────────────── */

/* GET /v1/users/me — validates the JWT (200 + the user object) without a CV
 * call. 401 ⇒ token missing/expired. */
static esp_err_t me_get(int *status, char *resp, char *detail, size_t dsz)
{
    char *auth = heap_caps_malloc(900, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!auth) { snprintf(detail, dsz, "out of memory"); return ESP_ERR_NO_MEM; }
    bearer(auth, 900);

    esp_http_client_config_t cfg = {
        .url               = INAT_ME_URL,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(auth); snprintf(detail, dsz, "http client init failed"); return ESP_FAIL; }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "User-Agent", INAT_UA);
    esp_http_client_set_header(c, "Accept", "application/json");

    esp_err_t ret = esp_http_client_open(c, 0);
    if (ret != ESP_OK) {
        char d[96];
        cu_tls_detail(c, d, sizeof(d));
        snprintf(detail, dsz, "%s; %s", esp_err_to_name(ret), d);
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int rd = 0, r;
    while (rd < INAT_RESP_MAX - 1 &&
           (r = esp_http_client_read(c, resp + rd, INAT_RESP_MAX - 1 - rd)) > 0)
        rd += r;
    resp[rd] = '\0';
    *status = esp_http_client_get_status_code(c);
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(auth);
    return ret;
}

esp_err_t inat_test(char *out, size_t osz)
{
    if (!inat_have_token()) { snprintf(out, osz, "no token stored"); return ESP_ERR_INVALID_STATE; }

    char ip[16] = "";
    if (!cu_resolve_host(INAT_API_HOST, ip, sizeof(ip))) {
        snprintf(out, osz, "DNS lookup for %s failed - no internet?", INAT_API_HOST);
        return ESP_FAIL;
    }
    if (strcmp(ip, "0.0.0.0") == 0) {
        snprintf(out, osz, "DNS returned 0.0.0.0 - a filtering resolver is blocking it");
        return ESP_FAIL;
    }
    char why[64] = "";
    if (!cu_tcp_probe(ip, 443, 8000, why, sizeof(why))) {
        snprintf(out, osz, "%s resolves to %s but TCP 443 is unreachable (%s)",
                 INAT_API_HOST, ip, why);
        return ESP_FAIL;
    }

    char *resp = heap_caps_malloc(INAT_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) { snprintf(out, osz, "out of memory"); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_FAIL;
    int status = 0;
    char detail[128] = "";
    esp_err_t err = me_get(&status, resp, detail, sizeof(detail));
    if (err != ESP_OK) {
        snprintf(out, osz, "TCP 443 to %s open but TLS failed - %s", ip, detail);
        goto out_done;
    }
    if (status == 200) {
        char login[48] = "";
        cu_json_str(resp, "login", login, sizeof(login));
        snprintf(out, osz, "OK - token valid%s%s", login[0] ? ", user " : "", login);
        ret = ESP_OK;
    } else if (status == 401) {
        snprintf(out, osz, "token rejected (HTTP 401) - expired or invalid; iNat JWTs "
                           "last ~24h, grab a fresh one from inaturalist.org/users/api_token");
    } else {
        char msg[80] = "";
        cu_json_str(resp, "error", msg, sizeof(msg));
        snprintf(out, osz, "reached %s but HTTP %d%s%s", ip, status, msg[0] ? " - " : "", msg);
    }
out_done:
    free(resp);
    snprintf(s_last_error, sizeof(s_last_error), "%s", ret == ESP_OK ? "" : out);
    return ret;
}
