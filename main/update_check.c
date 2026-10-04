/* Daily update check (FSD §8) — see update_check.h.
 *
 * WHY ON THE DEVICE, not in the browser: the OTA tab already lists releases
 * from the browser, but only when someone opens it and presses Reload. A
 * device-side check runs whether or not a page is open, and every viewer sees
 * the same answer on the very next /api/status poll.
 *
 * INTERNAL DRAM. A TLS handshake is the one thing here that needs internal
 * RAM, so nothing holds any between checks: a light esp_timer decides every
 * few minutes whether a check is due and only then spawns a short-lived worker
 * task, which deletes itself when done. No permanent 8 kB stack for a job that
 * runs once a day.
 *
 * CERT BUNDLE, not pinned roots. The bundle's CA callback leaks ~7 internal
 * blocks per handshake (v2.50) — that is why iNat, at hundreds of calls a day,
 * is pinned. At one call a day the leak is noise, and pinning GitHub's CA would
 * turn GitHub's next CA rotation into a silently dead check. otau_task (the OTA
 * from-GitHub download) makes the same call for the same reason.
 *
 * WALL CLOCK, kept in RTC memory. "Due" means 24 h since the last success by
 * the wall clock, stored RTC_DATA_ATTR so it survives deep-sleep wakes (night
 * mode 2 reboots the app on every probe and must not re-check each time) but
 * resets on a power-on or OTA reboot — after a flash a fresh check is wanted. */
#include "update_check.h"
#include "version.h"
#include "wifi.h"
#include "classify.h"
#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "updchk";

#define UPD_URL        "https://api.github.com/repos/SEspe/BirdBox/releases/latest"
/* GitHub requires a User-Agent. Deliberately WITHOUT the running version: the
 * comparison happens on the device, so the request need not say what it runs. */
#define UPD_UA         "BirdBox (ESP32 nest-box camera)"
#define UPD_RESP_MAX   16384          /* the reply is ~4.4 kB; tag_name and assets
                                         come before the release notes */
#define UPD_TICK_S     300            /* how often the timer asks "is one due?" */
#define UPD_PERIOD_S   (24 * 3600)    /* success -> next check */
#define UPD_RETRY_S    3600           /* failure -> next attempt */
#define UPD_VALID_YEAR 2020           /* TLS cannot verify a cert on the 1970 clock */

static RTC_DATA_ATTR time_t s_ok_epoch;    /* last successful check, 0 = never */
static RTC_DATA_ATTR time_t s_try_epoch;   /* last attempt, success or not */
static RTC_DATA_ATTR char   s_latest[16];  /* "1.2.0" */
static RTC_DATA_ATTR char   s_err[8];      /* "net", "h403", "parse", "nobin" */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_busy;
static esp_timer_handle_t s_timer;

static bool clock_valid(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year + 1900 >= UPD_VALID_YEAR;
}

/* "1.2.3" -> true and the three parts; anything else (including a "v" left
 * on, or a pre-release suffix) -> false, and is never offered as an update. */
static bool parse_semver(const char *s, int v[3])
{
    char tail;
    return sscanf(s, "%d.%d.%d%c", &v[0], &v[1], &v[2], &tail) == 3;
}

static bool newer_than_running(const char *tag)
{
    int a[3], b[3];
    if (!parse_semver(tag, a) || !parse_semver(FIRMWARE_VERSION, b)) return false;
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static void set_result(const char *latest, const char *err, bool ok)
{
    time_t now = time(NULL);
    portENTER_CRITICAL(&s_mux);
    /* strlcpy, not snprintf: nothing heavier than a copy under a spinlock. */
    if (latest) strlcpy(s_latest, latest, sizeof(s_latest));
    strlcpy(s_err, err, sizeof(s_err));
    s_try_epoch = now;
    if (ok) s_ok_epoch = now;
    portEXIT_CRITICAL(&s_mux);
}

/* Pull "tag_name":"v1.2.0" out of the reply without a JSON parser: the field
 * is a plain string and GitHub's API sends it compact. Tolerates whitespace
 * around the colon. Writes the tag with any leading 'v' stripped. */
static bool extract_tag(const char *body, char *out, size_t n)
{
    const char *p = strstr(body, "\"tag_name\"");
    if (!p) return false;
    p += strlen("\"tag_name\"");
    while (*p == ' ' || *p == ':') p++;
    if (*p++ != '"') return false;
    if (*p == 'v' || *p == 'V') p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;   /* JSON-safe by construction */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"' && i > 0;
}

static void upd_worker(void *arg)
{
    char *buf = heap_caps_malloc(UPD_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { set_result(NULL, "nomem", false); s_busy = false; vTaskDelete(NULL); return; }

    esp_http_client_config_t cfg = {
        .url               = UPD_URL,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(buf); set_result(NULL, "init", false); s_busy = false; vTaskDelete(NULL); return; }
    esp_http_client_set_header(c, "User-Agent", UPD_UA);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    int status = 0, rd = 0;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int r;
        while (rd < UPD_RESP_MAX - 1 &&
               (r = esp_http_client_read(c, buf + rd, UPD_RESP_MAX - 1 - rd)) > 0)
            rd += r;
        status = esp_http_client_get_status_code(c);
    }
    buf[rd] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    char tag[16], etag[8];
    if (err != ESP_OK) {
        set_result(NULL, "net", false);
        ESP_LOGW(TAG, "update check: %s", esp_err_to_name(err));
    } else if (status != 200) {
        snprintf(etag, sizeof(etag), "h%d", status % 1000);
        set_result(NULL, etag, false);
        ESP_LOGW(TAG, "update check: HTTP %d", status);
    } else if (!extract_tag(buf, tag, sizeof(tag))) {
        set_result(NULL, "parse", false);
        ESP_LOGW(TAG, "update check: no usable tag_name in %d bytes", rd);
    } else {
        /* Only offer a release the OTA tab can actually flash: it lists
         * releases by their .bin asset, so one without a .bin is not an
         * update this device can take. The assets array precedes the notes. */
        const char *as = strstr(buf, "\"assets\"");
        bool has_bin = as && strstr(as, ".bin\"");
        set_result(has_bin ? tag : NULL, has_bin ? "" : "nobin", has_bin);
        ESP_LOGI(TAG, "update check: latest release %s%s, running %s", tag,
                 has_bin ? "" : " (no .bin asset)", FIRMWARE_VERSION);
    }

    free(buf);
    s_busy = false;
    vTaskDelete(NULL);
}

/* Runs on the esp_timer task: decide only, never block. */
static void upd_tick(void *arg)
{
    if (s_busy) return;
    time_t now = time(NULL);
    if (!clock_valid(now) || !wifi_is_connected() || wifi_in_portal_mode()) return;
    /* Stay out of the way of the work that also needs a large internal block
     * for TLS, and of a firmware write. The next tick re-asks. */
    if (classify_busy() || web_server_ota_active()) return;

    bool due;
    portENTER_CRITICAL(&s_mux);
    /* A clock that moved backwards (manual set) makes the stamps meaningless. */
    if (s_ok_epoch > now || s_try_epoch > now) s_ok_epoch = s_try_epoch = 0;
    due = (s_ok_epoch == 0 || now - s_ok_epoch >= UPD_PERIOD_S) &&
          (s_try_epoch == 0 || now - s_try_epoch >= UPD_RETRY_S);
    portEXIT_CRITICAL(&s_mux);
    if (!due) return;

    s_busy = true;
    if (xTaskCreate(upd_worker, "updchk", 8192, NULL, 2, NULL) != pdPASS) {
        s_busy = false;
        set_result(NULL, "task", false);
    }
}

void update_check_start(void)
{
    const esp_timer_create_args_t a = { .callback = upd_tick, .name = "updchk" };
    if (esp_timer_create(&a, &s_timer) != ESP_OK ||
        esp_timer_start_periodic(s_timer, (uint64_t) UPD_TICK_S * 1000000) != ESP_OK)
        ESP_LOGE(TAG, "could not start the update-check timer");
}

void update_check_latest(char *out, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_latest, n);
    portEXIT_CRITICAL(&s_mux);
}

void update_check_error(char *out, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_err, n);
    portEXIT_CRITICAL(&s_mux);
}

bool update_check_available(void)
{
    char t[16];
    update_check_latest(t, sizeof(t));
    return t[0] && newer_than_running(t);
}

long update_check_age_s(void)
{
    portENTER_CRITICAL(&s_mux);
    time_t ok = s_ok_epoch;
    portEXIT_CRITICAL(&s_mux);
    if (!ok) return -1;
    time_t now = time(NULL);
    return now >= ok ? (long) (now - ok) : -1;
}

bool update_check_busy(void) { return s_busy; }
