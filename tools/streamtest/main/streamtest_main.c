/* streamtest — minimal "does this board work at all?" camera streamer,
 * with WiFi provisioning and OTA.
 *
 * Purpose: bring up a candidate ESP32-S3 CAM board whose PSRAM is dead. BirdBox
 * itself cannot run on such a board (its frame buffers and gallery label table
 * live in PSRAM), so a failing board used to be a black box — it boot-loops in
 * cpu_start long before anything serves a page. This image builds with PSRAM
 * disabled entirely and puts the camera frame buffer in internal DRAM, which
 * caps resolution but exercises everything else: sensor, XCLK/SCCB wiring, WiFi,
 * TCP, the HTTP stack and the board's power path.
 *
 *   streams      -> the board is fine apart from its PSRAM
 *   doesn't      -> the fault is wider than PSRAM
 *
 * WHY OTA IS HERE: PSRAM experiments can now be flashed over the network. This
 * image runs from a dual-OTA partition table with rollback enabled, and only
 * calls esp_ota_mark_app_valid_cancel_rollback() once the camera, WiFi and HTTP
 * server are all up. A PSRAM-enabled test build hangs in cpu_start, never
 * reaches that call, and so is reverted by the bootloader on the next boot —
 * the board comes back as this streamer with no serial cable involved.
 *
 * Serves:  GET  /            one-page viewer
 *          GET  /stream      multipart MJPEG
 *          GET  /health      chip / heap / frame counters, OTA slot
 *          GET  /wifi        network config page (scan + save)
 *          GET  /api/scan    nearby APs as JSON
 *          POST /wifi-save   ssid+pass -> NVS, reboots
 *          GET  /ota         firmware upload form
 *          POST /ota/upload  raw .bin body -> inactive slot, reboots
 *          POST /reboot      restart
 *
 * Not part of the BirdBox firmware — a standalone bringup tool, not on its
 * version/FSD track. Pin map is copied from main/board_config.h
 * (BOARD_ESP32S3_CAM_GENERIC); that file stays the source of truth.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_camera.h"

#include "wifi_creds.h"

static const char *TAG = "streamtest";

/* ── camera pin map: BOARD_ESP32S3_CAM_GENERIC (main/board_config.h) ───────── */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_Y9     16
#define CAM_PIN_Y8     17
#define CAM_PIN_Y7     18
#define CAM_PIN_Y6     12
#define CAM_PIN_Y5     10
#define CAM_PIN_Y4      8
#define CAM_PIN_Y3      9
#define CAM_PIN_Y2     11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

#define AP_SSID   "BirdBox-Test"
#define AP_PASS   "birdbox1234"
#define NVS_NS    "stcfg"

#define STA_MAX_TRIES 8

static volatile uint32_t s_frames = 0;
static volatile uint32_t s_errors = 0;
static bool  s_ap_mode  = false;
static char  s_ip[16]   = "-";
static int   s_tries    = 0;

/* ── settings (NVS) ────────────────────────────────────────────────────────── */

/* Credentials live in NVS so the board can be re-pointed at another network
 * from the browser. wifi_creds.h is only the first-boot seed. */
static void cfg_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    ssid[0] = pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = ssid_sz; nvs_get_str(h, "ssid", ssid, &n);
        n = pass_sz;        nvs_get_str(h, "pass", pass, &n);
        nvs_close(h);
    }
    if (!ssid[0]) {                       /* fall back to the compiled-in seed */
        strlcpy(ssid, TEST_WIFI_SSID, ssid_sz);
        strlcpy(pass, TEST_WIFI_PASS, pass_sz);
    }
}

static esp_err_t cfg_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* ── camera ────────────────────────────────────────────────────────────────── */

/* No PSRAM means the frame buffer competes with WiFi for ~300 KB of DRAM, so
 * QVGA JPEG (~5-20 KB/frame) with a single buffer is the safe default. VGA
 * usually also fits; SVGA and up fail to allocate. */
static esp_err_t camera_start(void)
{
    camera_config_t cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_Y9, .pin_d6 = CAM_PIN_Y8,
        .pin_d5 = CAM_PIN_Y7, .pin_d4 = CAM_PIN_Y6,
        .pin_d3 = CAM_PIN_Y5, .pin_d2 = CAM_PIN_Y4,
        .pin_d1 = CAM_PIN_Y3, .pin_d0 = CAM_PIN_Y2,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count     = 1,
        .fb_location  = CAMERA_FB_IN_DRAM,   /* the whole point — no PSRAM */
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "free DRAM at failure: %u B, largest block %u B",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) ESP_LOGI(TAG, "sensor PID 0x%02x up, QVGA JPEG in DRAM", s->id.PID);
    return ESP_OK;
}

/* ── shared page furniture ─────────────────────────────────────────────────── */

static const char CSS[] =
    "<style>body{background:#111;color:#eee;font:14px system-ui;margin:0;padding:12px;"
    "text-align:center}img{max-width:100%;border:1px solid #444;border-radius:6px}"
    "a{color:#6cf;margin:0 8px}input,button,select{font:14px system-ui;padding:6px;"
    "margin:4px;border-radius:4px;border:1px solid #555;background:#222;color:#eee}"
    "button{cursor:pointer}table{margin:0 auto;border-collapse:collapse}"
    "td{padding:3px 8px;text-align:left}.nav{margin:10px 0;font-size:13px}</style>";
static const char NAV[] =
    "<div class=nav><a href='/'>stream</a><a href='/wifi'>wifi</a>"
    "<a href='/ota'>ota</a><a href='/health'>health</a></div>";

/* ── HTTP: stream + viewer ─────────────────────────────────────────────────── */

#define BOUNDARY "birdboxframe"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" BOUNDARY;
static const char *STREAM_SEP  = "\r\n--" BOUNDARY "\r\n";
static const char *STREAM_HDR  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>BirdBox board test</title>");
    httpd_resp_sendstr_chunk(req, CSS);
    httpd_resp_sendstr_chunk(req, "</head><body><h2>BirdBox board test</h2>");
    httpd_resp_sendstr_chunk(req, NAV);
    httpd_resp_sendstr_chunk(req,
        "<p>PSRAM is disabled in this build. A picture here means the sensor, WiFi and "
        "HTTP stack all work and PSRAM is the board&#39;s only defect.</p>"
        "<img src='/stream' alt='stream'></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_stream(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
    if (res != ESP_OK) return res;

    char hdr[64];
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            s_errors++;
            ESP_LOGW(TAG, "fb_get returned NULL (%u so far)", (unsigned) s_errors);
            /* Don't kill the connection on one bad grab — the sensor can miss a
             * frame; only a client disconnect should end the stream. */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int n = snprintf(hdr, sizeof(hdr), STREAM_HDR, (unsigned) fb->len);
        res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, n);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *) fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) {            /* client went away */
            ESP_LOGI(TAG, "stream client closed after %u frames", (unsigned) s_frames);
            return ESP_OK;
        }
        s_frames++;
    }
}

static esp_err_t h_health(httpd_req_t *req)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t fsz = 0, fid = 0;
    esp_flash_get_size(NULL, &fsz);
    esp_flash_read_id(NULL, &fid);
    sensor_t *s = esp_camera_sensor_get();
    const esp_partition_t *run = esp_ota_get_running_partition();

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "streamtest\n"
        "chip         : ESP32-S3 rev v%u.%u, %u cores\n"
        "embPsramBit  : %s  (unreliable — disagrees with esptool, do not use for ID)\n"
        "psram        : DISABLED in this build (intentional)\n"
        "flash        : %u MB, id 0x%06lx\n"
        "sensor PID   : 0x%04x\n"
        "mode         : %s\n"
        "ip           : %s\n"
        "frames sent  : %u\n"
        "fb failures  : %u\n"
        "free DRAM    : %u B (largest block %u B)\n"
        "ota slot     : %s\n"
        "uptime       : %u s\n",
        (unsigned) (ci.revision / 100), (unsigned) (ci.revision % 100),
        (unsigned) ci.cores,
        (ci.features & CHIP_FEATURE_EMB_PSRAM) ? "yes" : "no",
        (unsigned) (fsz / (1024 * 1024)), (unsigned long) fid,
        s ? s->id.PID : 0,
        s_ap_mode ? "SoftAP" : "STA",
        s_ip,
        (unsigned) s_frames, (unsigned) s_errors,
        (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        run ? run->label : "?",
        (unsigned) (esp_log_timestamp() / 1000));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, n);
}

/* ── HTTP: WiFi config ─────────────────────────────────────────────────────── */

static esp_err_t h_wifi_page(httpd_req_t *req)
{
    char ssid[33], pass[65];
    cfg_load(ssid, sizeof(ssid), pass, sizeof(pass));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>WiFi</title>");
    httpd_resp_sendstr_chunk(req, CSS);
    httpd_resp_sendstr_chunk(req, "</head><body><h2>WiFi</h2>");
    httpd_resp_sendstr_chunk(req, NAV);

    char cur[220];
    snprintf(cur, sizeof(cur),
        "<p>Mode <b>%s</b>, IP <b>%s</b>, saved SSID <b>%s</b></p>",
        s_ap_mode ? "SoftAP" : "STA", s_ip, ssid[0] ? ssid : "(none)");
    httpd_resp_sendstr_chunk(req, cur);

    httpd_resp_sendstr_chunk(req,
        "<button onclick='scan()'>Scan networks</button><div id=nets></div>"
        "<form method='POST' action='/wifi-save'>"
        "<div><input name='ssid' id='ssid' placeholder='SSID' required></div>"
        "<div><input name='pass' type='password' placeholder='password'></div>"
        "<button type='submit'>Save &amp; reboot</button></form>"
        "<p style='font-size:12px;color:#999'>Saved to NVS. Leaving SSID blank is not "
        "allowed here; to force SoftAP, reflash with an empty TEST_WIFI_SSID.</p>"
        "<script>function scan(){var d=document.getElementById('nets');"
        "d.textContent='scanning\\u2026';"
        "fetch('/api/scan').then(r=>r.json()).then(function(a){var h='<table>';"
        "a.forEach(function(n){h+=\"<tr><td><a href='#' onclick=\\\"document."
        "getElementById('ssid').value='\"+n.ssid+\"';return false\\\">\"+n.ssid+"
        "'</a></td><td>'+n.rssi+' dBm</td></tr>';});"
        "d.innerHTML=h+'</table>';}).catch(function(){d.textContent='scan failed';});}"
        "</script></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < n; i++) {
        /* Quotes/backslashes in an SSID would break the JSON — skip those rather
         * than ship a half-written escaper into a bringup tool. */
        if (strchr((char *) recs[i].ssid, '"') || strchr((char *) recs[i].ssid, '\\'))
            continue;
        char one[96];
        snprintf(one, sizeof(one), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i ? "," : "", (char *) recs[i].ssid, recs[i].rssi);
        httpd_resp_sendstr_chunk(req, one);
    }
    httpd_resp_sendstr_chunk(req, "]");
    free(recs);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Tiny form-field reader: enough for ssid/pass, no percent-decoding beyond '+'. */
static void form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < out_sz) {
        if (*p == '+') { out[i++] = ' '; p++; }
        else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            out[i++] = (char) strtol(hex, NULL, 16);
            p += 3;
        } else out[i++] = *p++;
    }
    out[i] = '\0';
}

static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len < (int) sizeof(body) - 1 ? req->content_len
                                                        : (int) sizeof(body) - 1;
    int got = len > 0 ? httpd_req_recv(req, body, len) : 0;
    if (got <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_OK; }
    body[got] = '\0';

    char ssid[33], pass[65];
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    if (!ssid[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required"); return ESP_OK; }

    if (cfg_save(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "saved SSID \"%s\" — rebooting", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<body style='background:#111;color:#eee;font:14px system-ui'>"
                            "Saved. Rebooting &mdash; reconnect on the new network.</body>");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

/* ── HTTP: OTA ─────────────────────────────────────────────────────────────── */

static esp_err_t h_ota_page(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>OTA</title>");
    httpd_resp_sendstr_chunk(req, CSS);
    httpd_resp_sendstr_chunk(req, "</head><body><h2>Firmware upload</h2>");
    httpd_resp_sendstr_chunk(req, NAV);
    char cur[160];
    snprintf(cur, sizeof(cur), "<p>Running from <b>%s</b></p>", run ? run->label : "?");
    httpd_resp_sendstr_chunk(req, cur);
    httpd_resp_sendstr_chunk(req,
        "<p style='font-size:13px;color:#999;max-width:32em;margin:0 auto'>Rollback is "
        "enabled: an image that fails to reach its &ldquo;mark valid&rdquo; call &mdash; "
        "which is what a PSRAM-enabled build does on this board, hanging in "
        "<code>cpu_start</code> &mdash; is reverted by the bootloader on the next boot. "
        "So a failed PSRAM experiment costs a reboot, not a serial cable.</p>"
        "<div><input type='file' id='f' accept='.bin'></div>"
        "<button onclick='up()'>Upload &amp; reboot</button><div id='p'></div>"
        "<script>function up(){var f=document.getElementById('f').files[0];"
        "var p=document.getElementById('p');if(!f){p.textContent='pick a .bin first';return;}"
        "var x=new XMLHttpRequest();x.open('POST','/ota/upload',true);"
        "x.setRequestHeader('Content-Type','application/octet-stream');"
        "x.upload.onprogress=function(e){if(e.lengthComputable)"
        "p.textContent='uploading '+Math.round(e.loaded/e.total*100)+'%';};"
        "x.onload=function(){p.textContent=x.status===200?'ok \\u2014 rebooting':"
        "('error: '+x.responseText);};"
        "x.onerror=function(){p.textContent='upload error';};x.send(f);}"
        "</script></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA slot"); return ESP_OK; }
    if (req->content_len <= 0 || (size_t) req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or oversized upload");
        return ESP_OK;
    }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len, retries = 0;
    bool ok = true;
    while (remaining > 0) {
        int want = remaining < (int) sizeof(buf) ? remaining : (int) sizeof(buf);
        int got  = httpd_req_recv(req, buf, want);
        if (got <= 0) {
            /* One dropped packet must not abort a ~1 MB upload. */
            if ((got == HTTPD_SOCK_ERR_TIMEOUT) && ++retries < 10) continue;
            ok = false; break;
        }
        retries = 0;
        if (esp_ota_write(ota, buf, got) != ESP_OK) { ok = false; break; }
        remaining -= got;
    }

    if (!ok) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_OK;
    }
    if (esp_ota_end(ota) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "finalize failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA written to %s — rebooting", part->label);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ── HTTP server ───────────────────────────────────────────────────────────── */

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 6144;
    cfg.lru_purge_enable = true;
    /* Default cap is 8 and we register 9 — registrations past the cap are
     * silently dropped and the route just 404s (a BirdBox lesson). */
    cfg.max_uri_handlers = 16;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }

    const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = h_index      },
        { .uri = "/stream",     .method = HTTP_GET,  .handler = h_stream     },
        { .uri = "/health",     .method = HTTP_GET,  .handler = h_health     },
        { .uri = "/wifi",       .method = HTTP_GET,  .handler = h_wifi_page  },
        { .uri = "/api/scan",   .method = HTTP_GET,  .handler = h_scan       },
        { .uri = "/wifi-save",  .method = HTTP_POST, .handler = h_wifi_save  },
        { .uri = "/ota",        .method = HTTP_GET,  .handler = h_ota_page   },
        { .uri = "/ota/upload", .method = HTTP_POST, .handler = h_ota_upload },
        { .uri = "/reboot",     .method = HTTP_POST, .handler = h_reboot     },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(srv, &routes[i]) == ESP_OK) n++;
        else ESP_LOGE(TAG, "route %s failed to register", routes[i].uri);
    }
    ESP_LOGI(TAG, "HTTP server up (%d routes)", n);
}

/* ── WiFi ──────────────────────────────────────────────────────────────────── */

static void ap_start(void);

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_mode) return;
        if (++s_tries > STA_MAX_TRIES) {
            ESP_LOGW(TAG, "could not join after %d tries — falling back to SoftAP",
                     STA_MAX_TRIES);
            ap_start();
            return;
        }
        ESP_LOGW(TAG, "disconnected (try %d/%d) — retrying", s_tries, STA_MAX_TRIES);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;
        s_tries = 0;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        /* Printed loudly because serial is the only way to learn the address on
         * a fresh board. */
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "  STREAM READY:  http://%s/", s_ip);
        ESP_LOGI(TAG, "==================================================");
    }
}

/* SoftAP fallback, also used when no SSID is configured. APSTA (not plain AP)
 * so the config page's network scan still works. */
static void ap_start(void)
{
    s_ap_mode = true;
    strlcpy(s_ip, "192.168.4.1", sizeof(s_ip));
    esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {0};
    strlcpy((char *) wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid));
    strlcpy((char *) wc.ap.password, AP_PASS, sizeof(wc.ap.password));
    wc.ap.ssid_len       = strlen(AP_SSID);
    wc.ap.max_connection = 2;
    wc.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    ESP_LOGI(TAG, "SoftAP \"%s\" (pass %s) — join it, then http://192.168.4.1/",
             AP_SSID, AP_PASS);
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi, NULL, NULL));
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    char ssid[33], pass[65];
    cfg_load(ssid, sizeof(ssid), pass, sizeof(pass));

    esp_netif_create_default_wifi_sta();
    if (ssid[0]) {
        wifi_config_t wc = {0};
        strlcpy((char *) wc.sta.ssid,     ssid, sizeof(wc.sta.ssid));
        strlcpy((char *) wc.sta.password, pass, sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_LOGI(TAG, "joining \"%s\" — watch for the STREAM READY line", ssid);
    } else {
        ap_start();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ── main ──────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "streamtest — PSRAM-free board bringup (OTA + WiFi config)");
    ESP_LOGI(TAG, "free DRAM at boot: %u B",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Camera first: if it cannot allocate its buffer, that is the headline
     * result. Note we do NOT return early — the network still comes up so the
     * board stays reachable for OTA even with a dead sensor. */
    bool cam_ok = (camera_start() == ESP_OK);
    if (!cam_ok) ESP_LOGE(TAG, "camera init FAILED — /stream will not work, "
                               "bringing up network anyway for OTA");

    wifi_start();
    http_start();

    /* Rollback gate. Only confirm this image once the pieces that matter are
     * actually running, so a broken build gets reverted instead of sticking.
     * The camera is deliberately NOT required: a board with a dead sensor must
     * still keep OTA reachable, or it becomes unrecoverable without serial. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "marking image valid (cancels rollback)");
        esp_ota_mark_app_valid_cancel_rollback();
    }
    ESP_LOGI(TAG, "ready — camera %s, mode %s", cam_ok ? "ok" : "FAILED",
             s_ap_mode ? "SoftAP" : "STA");
}
