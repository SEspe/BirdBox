/* Shared plumbing for the cloud species-ID providers — see cloud_util.h for why
 * this exists. Bodies here were lifted verbatim out of claude.c so both the
 * Claude and Gemini request paths share one copy. */
#include "cloud_util.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#define B64_CHUNK_IN   3072            /* multiple of 3 => only the final chunk
                                          pads, so the stream is a valid single
                                          base64 document */
#define B64_CHUNK_OUT  (B64_CHUNK_IN / 3 * 4 + 4)

const char *cu_json_seek(const char *j, const char *key)
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
 * resolving escapes. */
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

bool cu_json_str(const char *j, const char *key, char *out, size_t osz)
{
    return json_str_at(cu_json_seek(j, key), out, osz);
}

long cu_json_num(const char *j, const char *key, long def)
{
    const char *p = cu_json_seek(j, key);
    if (!p) return def;
    char *end;
    long v = strtol(p, &end, 10);
    return end == p ? def : v;
}

bool cu_json_bool(const char *j, const char *key, bool def)
{
    const char *p = cu_json_seek(j, key);
    if (!p) return def;
    if (strncmp(p, "true",  4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

void cu_scrub(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\\' || *p == '\n' || *p == '\r')
            *p = ' ';
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = '\0';
}

void cu_to_result(classify_result_t *out, bool bird, const char *common,
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

void cu_tls_detail(esp_http_client_handle_t c, char *out, size_t osz)
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

bool cu_resolve_host(const char *host, char *ip, size_t n)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, "443", &hints, &res) != 0 || !res) return false;
    struct sockaddr_in *a = (struct sockaddr_in *) res->ai_addr;
    inet_ntop(AF_INET, &a->sin_addr, ip, n);
    freeaddrinfo(res);
    return true;
}

bool cu_tcp_probe(const char *ip, int port, int timeout_ms, char *why, size_t wsz)
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

size_t cu_b64_len(size_t n)
{
    return 4 * ((n + 2) / 3);
}

bool cu_write(esp_http_client_handle_t c, const char *b, size_t n)
{
    while (n) {
        int w = esp_http_client_write(c, b, n);
        if (w <= 0) return false;
        b += w;
        n -= w;
    }
    return true;
}

bool cu_status_transient(int status)
{
    return status == 429 || status == 500 || status == 502 ||
           status == 503 || status == 504;
}

void cu_retry_backoff(int attempt)
{
    /* Exponential: 2 s, 4 s, 8 s. Matches Google's Gemini free-tier guidance for
     * riding out a 429 (10 RPM) — a fixed rate-limit window clears in under a
     * minute, so a widening backoff catches it without hammering. Capped so a
     * stray high `attempt` can't overflow into a multi-minute sleep. */
    if (attempt > 3) attempt = 3;   /* cap at 16 s */
    vTaskDelay(pdMS_TO_TICKS(2000 << attempt));
}

bool cu_stream_b64(esp_http_client_handle_t c, const uint8_t *data, size_t len)
{
    /* ~4 KB, off the (tight) caller stack and out of internal RAM. */
    uint8_t *chunk = heap_caps_malloc(B64_CHUNK_OUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) return false;

    bool ok = true;
    for (size_t off = 0; off < len && ok; off += B64_CHUNK_IN) {
        size_t n  = (len - off < B64_CHUNK_IN) ? len - off : B64_CHUNK_IN;
        size_t ol = 0;
        if (mbedtls_base64_encode(chunk, B64_CHUNK_OUT, &ol, data + off, n) != 0)
            ok = false;
        else
            ok = cu_write(c, (const char *) chunk, ol);
    }
    free(chunk);
    return ok;
}
