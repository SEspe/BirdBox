/* streamtest — "does this board work at all?" camera tool for ESP32-S3 CAM
 * boards, with a tabbed web UI, OTA and WiFi provisioning.
 *
 * Purpose: bring up a candidate board whose PSRAM is dead. BirdBox itself cannot
 * run on such a board (its frame buffers and gallery label table live in PSRAM),
 * so a failing board used to be a black box — it boot-loops in cpu_start long
 * before anything serves a page. This image builds with PSRAM disabled entirely
 * and puts the camera frame buffer in internal DRAM, which caps resolution but
 * exercises everything else: sensor, XCLK/SCCB wiring, WiFi, TCP, the HTTP stack
 * and the board's power path.
 *
 *   streams      -> the board is fine apart from its PSRAM
 *   doesn't      -> the fault is wider than PSRAM
 *
 * WHY OTA IS HERE: PSRAM experiments can be flashed over the network. This image
 * runs from a dual-OTA partition table with rollback enabled and only calls
 * esp_ota_mark_app_valid_cancel_rollback() once WiFi and HTTP are up. A
 * PSRAM-enabled test build hangs in cpu_start, never reaches that call, and is
 * reverted by the bootloader on the next boot — no serial cable involved.
 *
 * UI is five tabs, mirroring BirdBox's layout: Live, Settings, Debug, WiFi, OTA.
 * As in BirdBox, the whole UI is one inline <script> assembled from C string
 * literals, so a single JS syntax error kills every handler while the live
 * <img src=/stream> keeps working (it's HTML, not JS) — that's the tell. The C
 * compiler cannot catch it; verify by grepping the served page.
 *
 * Routes: GET  /            tabbed UI
 *         GET  /stream      multipart MJPEG
 *         GET  /health      plain-text summary (for curl)
 *         GET  /api/sysinfo Debug tab JSON
 *         GET  /api/cam     camera settings + AF state JSON
 *         POST /api/ctl     ?k=<control>&v=<value>  (k=framesize re-inits)
 *         POST /api/af      ?a=trigger|auto|manual
 *         GET  /api/scan    nearby APs
 *         POST /wifi-save   ssid+pass -> NVS, reboots
 *         POST /ota/upload  raw .bin -> inactive slot, reboots
 *         POST /reboot      restart
 *
 * Not part of the BirdBox firmware — a standalone bringup tool, not on its
 * version/FSD track. Pin map is copied from main/board_config.h
 * (BOARD_ESP32S3_CAM_GENERIC); that file stays the source of truth.
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* OV5640 extras. s_af_loaded means the AF firmware blob was accepted by the
 * sensor's MCU — NOT that a focus motor exists (see sdkconfig.defaults). */
static framesize_t s_fs        = FRAMESIZE_QVGA;
static bool        s_af_loaded = false;
static const char *s_af_note   = "not attempted";
/* Re-initialising the camera while a stream handler is inside fb_get would use
 * freed buffers, so resolution changes are refused while a client is attached.
 * The UI drops the stream image when you leave the Live tab, which releases it. */
static volatile int s_stream_clients = 0;

/* Resolutions worth trying. The big ones are expected to fail without PSRAM —
 * that failure is the useful result, so they stay listed. */
static const struct { const char *name; framesize_t fs; } FRAMESIZES[] = {
    { "QQVGA", FRAMESIZE_QQVGA }, { "QVGA", FRAMESIZE_QVGA },
    { "CIF",   FRAMESIZE_CIF   }, { "VGA",  FRAMESIZE_VGA  },
    { "SVGA",  FRAMESIZE_SVGA  }, { "XGA",  FRAMESIZE_XGA  },
    { "HD",    FRAMESIZE_HD    }, { "SXGA", FRAMESIZE_SXGA },
    { "UXGA",  FRAMESIZE_UXGA  }, { "QXGA", FRAMESIZE_QXGA },
    { "QSXGA", FRAMESIZE_QSXGA },
};
#define FRAMESIZES_N (sizeof(FRAMESIZES) / sizeof(FRAMESIZES[0]))

static const char *fs_name(framesize_t fs)
{
    for (size_t i = 0; i < FRAMESIZES_N; i++)
        if (FRAMESIZES[i].fs == fs) return FRAMESIZES[i].name;
    return "?";
}

/* ── settings (NVS) ────────────────────────────────────────────────────────── */

/* Credentials live in NVS so the board can be re-pointed at another network from
 * the browser. wifi_creds.h is only the first-boot seed. */
static void cfg_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    ssid[0] = pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = ssid_sz; nvs_get_str(h, "ssid", ssid, &n);
        n = pass_sz;        nvs_get_str(h, "pass", pass, &n);
        nvs_close(h);
    }
    if (!ssid[0]) {
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
 * QVGA JPEG with a single buffer is the safe default. */
static esp_err_t camera_start(framesize_t fs)
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
        .frame_size   = fs,
        .jpeg_quality = 12,
        .fb_count     = 1,
        .fb_location  = CAMERA_FB_IN_DRAM,   /* the whole point — no PSRAM */
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init(%s) failed: %s (0x%x)",
                 fs_name(fs), esp_err_to_name(err), err);
        ESP_LOGE(TAG, "free DRAM at failure: %u B, largest block %u B",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return err;
    }
    s_fs = fs;
    sensor_t *s = esp_camera_sensor_get();
    if (s) ESP_LOGI(TAG, "sensor PID 0x%04x up, %s JPEG in DRAM", s->id.PID, fs_name(fs));
    return ESP_OK;
}

/* Load the OV5640 AF firmware into the sensor's on-chip MCU. Best-effort: a
 * failure here must never stop the stream, it just means no autofocus. */
static void af_setup(void)
{
    s_af_loaded = false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) { s_af_note = "no sensor"; return; }
    if (!s->af_is_supported || !s->af_is_supported(s) || !s->af_init) {
        s_af_note = "no AF support in this driver build";
        return;
    }
    if (s->af_init(s, 3000) == 0) {
        s_af_loaded = true;
        /* Deliberately hedged: loading proves the sensor MCU accepted the blob,
         * NOT that a focus motor is fitted. af_is_supported() returns 1 for
         * every OV5640, so software cannot tell fixed-focus from VCM modules. */
        s_af_note = "firmware loaded - moves a lens only if the module has a VCM";
    } else {
        s_af_note = "firmware load FAILED";
    }
    ESP_LOGI(TAG, "autofocus: %s", s_af_note);
}

/* Re-init at a new resolution. Falls back to the previous size if the new one
 * cannot allocate, so a too-ambitious pick never costs us the camera. */
static esp_err_t camera_set_framesize(framesize_t fs, char *msg, size_t msg_sz)
{
    if (s_stream_clients > 0) {
        snprintf(msg, msg_sz, "refused: %d stream client(s) attached - leave the "
                              "Live tab first", s_stream_clients);
        return ESP_ERR_INVALID_STATE;
    }
    framesize_t prev = s_fs;
    esp_camera_deinit();
    if (camera_start(fs) == ESP_OK) {
        af_setup();                        /* AF firmware does not survive re-init */
        snprintf(msg, msg_sz, "ok: %s (%u B DRAM free, largest block %u B)",
                 fs_name(fs),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_OK;
    }
    if (camera_start(prev) == ESP_OK) {
        af_setup();
        snprintf(msg, msg_sz, "%s would not allocate without PSRAM - reverted to %s",
                 fs_name(fs), fs_name(prev));
    } else {
        snprintf(msg, msg_sz, "%s failed AND revert to %s failed - reboot needed",
                 fs_name(fs), fs_name(prev));
    }
    return ESP_FAIL;
}

/* One place mapping a control name to its setter, so the HTTP layer stays dumb.
 * Returns 0 ok, -1 setter rejected/absent, -2 unknown key. */
static int ctl_set(sensor_t *s, const char *k, int v)
{
    #define CTL(name, fn) if (!strcmp(k, name)) return s->fn ? s->fn(s, v) : -1
    CTL("brightness",     set_brightness);
    CTL("contrast",       set_contrast);
    CTL("saturation",     set_saturation);
    CTL("sharpness",      set_sharpness);
    CTL("denoise",        set_denoise);
    CTL("quality",        set_quality);
    CTL("special_effect", set_special_effect);
    CTL("wb_mode",        set_wb_mode);
    CTL("awb",            set_whitebal);
    CTL("awb_gain",       set_awb_gain);
    CTL("aec",            set_exposure_ctrl);
    CTL("aec2",           set_aec2);
    CTL("ae_level",       set_ae_level);
    CTL("aec_value",      set_aec_value);
    CTL("agc",            set_gain_ctrl);
    CTL("agc_gain",       set_agc_gain);
    CTL("bpc",            set_bpc);
    CTL("wpc",            set_wpc);
    CTL("raw_gma",        set_raw_gma);
    CTL("lenc",           set_lenc);
    CTL("hmirror",        set_hmirror);
    CTL("vflip",          set_vflip);
    CTL("dcw",            set_dcw);
    CTL("colorbar",       set_colorbar);
    #undef CTL
    return -2;
}

/* ── MJPEG stream ──────────────────────────────────────────────────────────── */

#define BOUNDARY "birdboxframe"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" BOUNDARY;
static const char *STREAM_SEP  = "\r\n--" BOUNDARY "\r\n";
static const char *STREAM_HDR  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t h_stream(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
    if (res != ESP_OK) return res;

    s_stream_clients++;                    /* blocks resolution re-init */
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
        if (res != ESP_OK) {               /* client went away */
            ESP_LOGI(TAG, "stream client closed after %u frames", (unsigned) s_frames);
            s_stream_clients--;
            return ESP_OK;
        }
        s_frames++;
    }
}

/* ── query helpers ─────────────────────────────────────────────────────────── */

static bool q_str(httpd_req_t *req, const char *key, char *out, size_t out_sz)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, key, out, out_sz) == ESP_OK;
}

static bool q_int(httpd_req_t *req, const char *key, int *out)
{
    char v[24];
    if (!q_str(req, key, v, sizeof(v))) return false;
    char *end = NULL;
    long l = strtol(v, &end, 10);
    if (end == v) return false;
    *out = (int) l;
    return true;
}

/* ── JSON: camera settings + AF ────────────────────────────────────────────── */

static esp_err_t h_cam_json(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    httpd_resp_set_type(req, "application/json");
    if (!s) return httpd_resp_sendstr(req, "{\"error\":\"no sensor\"}");

    camera_status_t *st = &s->status;
    uint8_t raw = 0; bool focused = false, busy = false;
    bool have_af = s_af_loaded && s->af_get_status &&
                   s->af_get_status(s, &raw, &focused, &busy) == 0;

    char buf[900];
    int n = snprintf(buf, sizeof(buf),
        "{\"pid\":%u,\"framesize\":\"%s\","
        "\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"sharpness\":%d,"
        "\"denoise\":%u,\"quality\":%u,\"special_effect\":%u,\"wb_mode\":%u,"
        "\"awb\":%u,\"awb_gain\":%u,\"aec\":%u,\"aec2\":%u,\"ae_level\":%d,"
        "\"aec_value\":%u,\"agc\":%u,\"agc_gain\":%u,\"bpc\":%u,\"wpc\":%u,"
        "\"raw_gma\":%u,\"lenc\":%u,\"hmirror\":%u,\"vflip\":%u,\"dcw\":%u,"
        "\"colorbar\":%u,"
        "\"afLoaded\":%s,\"afNote\":\"%s\",\"afRaw\":%u,\"afFocused\":%s,"
        "\"afBusy\":%s,\"afValid\":%s,\"streamClients\":%d}",
        (unsigned) s->id.PID, fs_name(s_fs),
        st->brightness, st->contrast, st->saturation, st->sharpness,
        st->denoise, st->quality, st->special_effect, st->wb_mode,
        st->awb, st->awb_gain, st->aec, st->aec2, st->ae_level,
        st->aec_value, st->agc, st->agc_gain, st->bpc, st->wpc,
        st->raw_gma, st->lenc, st->hmirror, st->vflip, st->dcw,
        st->colorbar,
        s_af_loaded ? "true" : "false", s_af_note, raw,
        focused ? "true" : "false", busy ? "true" : "false",
        have_af ? "true" : "false", s_stream_clients);
    return httpd_resp_send(req, buf, n);
}

/* ── JSON: Debug tab ───────────────────────────────────────────────────────── */

static esp_err_t h_sysinfo(httpd_req_t *req)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t fsz = 0, fid = 0;
    esp_flash_get_size(NULL, &fsz);
    esp_flash_read_id(NULL, &fid);
    sensor_t *s = esp_camera_sensor_get();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *ad = esp_app_get_description();
    char ssid[33], pass[65];
    cfg_load(ssid, sizeof(ssid), pass, sizeof(pass));
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (!s_ap_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    char buf[1100];
    int n = snprintf(buf, sizeof(buf),
        "{\"chipModel\":\"%s\",\"chipRev\":\"v%u.%u\",\"chipCores\":%u,"
        "\"embPsramBit\":%s,\"psram\":\"disabled in this build (intentional)\","
        "\"cpuMhz\":%d,\"flashMB\":%u,\"flashId\":\"0x%06lx\","
        "\"dram\":%u,\"dramBig\":%u,"
        "\"sensorPid\":\"0x%04x\",\"framesize\":\"%s\",\"afNote\":\"%s\","
        "\"frames\":%u,\"fbErrors\":%u,\"streamClients\":%d,"
        "\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,"
        "\"otaSlot\":\"%s\",\"idfVer\":\"%s\",\"buildDate\":\"%s %s\","
        "\"uptime\":%u}",
        CONFIG_IDF_TARGET,
        (unsigned) (ci.revision / 100), (unsigned) (ci.revision % 100),
        (unsigned) ci.cores,
        (ci.features & CHIP_FEATURE_EMB_PSRAM) ? "true" : "false",
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (unsigned) (fsz / (1024 * 1024)), (unsigned long) fid,
        (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        s ? s->id.PID : 0, fs_name(s_fs), s_af_note,
        (unsigned) s_frames, (unsigned) s_errors, s_stream_clients,
        s_ap_mode ? "SoftAP" : "STA", s_ip, ssid, rssi,
        run ? run->label : "?",
        ad ? ad->idf_ver : "?", ad ? ad->date : "?", ad ? ad->time : "?",
        (unsigned) (esp_log_timestamp() / 1000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* Plain-text summary, kept for curl / scripted checks. */
static esp_err_t h_health(httpd_req_t *req)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    sensor_t *s = esp_camera_sensor_get();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char buf[600];
    int n = snprintf(buf, sizeof(buf),
        "streamtest\n"
        "chip         : %s rev v%u.%u, %u cores\n"
        "psram        : DISABLED in this build (intentional)\n"
        "sensor PID   : 0x%04x\n"
        "framesize    : %s\n"
        "autofocus    : %s\n"
        "mode         : %s\nip           : %s\n"
        "frames sent  : %u\nfb failures  : %u\nstream clients: %d\n"
        "free DRAM    : %u B (largest block %u B)\n"
        "ota slot     : %s\nuptime       : %u s\n",
        CONFIG_IDF_TARGET,
        (unsigned) (ci.revision / 100), (unsigned) (ci.revision % 100),
        (unsigned) ci.cores,
        s ? s->id.PID : 0, fs_name(s_fs), s_af_note,
        s_ap_mode ? "SoftAP" : "STA", s_ip,
        (unsigned) s_frames, (unsigned) s_errors, s_stream_clients,
        (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        run ? run->label : "?", (unsigned) (esp_log_timestamp() / 1000));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, n);
}

/* ── control endpoints ─────────────────────────────────────────────────────── */

static esp_err_t h_ctl(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sensor"); return ESP_OK; }

    char k[24];
    if (!q_str(req, "k", k, sizeof(k))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "k= required");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");

    if (!strcmp(k, "framesize")) {
        char name[12];
        if (!q_str(req, "v", name, sizeof(name))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "v=<SIZE> required");
            return ESP_OK;
        }
        for (size_t i = 0; i < FRAMESIZES_N; i++) {
            if (!strcasecmp(name, FRAMESIZES[i].name)) {
                char msg[176];
                camera_set_framesize(FRAMESIZES[i].fs, msg, sizeof(msg));
                return httpd_resp_sendstr(req, msg);
            }
        }
        return httpd_resp_sendstr(req, "unknown framesize");
    }

    int v = 0;
    if (!q_int(req, "v", &v)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "v=<int> required");
        return ESP_OK;
    }
    int r = ctl_set(s, k, v);
    char msg[96];
    if (r == 0)       snprintf(msg, sizeof(msg), "ok: %s = %d", k, v);
    else if (r == -2) snprintf(msg, sizeof(msg), "unknown control: %s", k);
    else              snprintf(msg, sizeof(msg), "sensor rejected %s = %d", k, v);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t h_af(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    httpd_resp_set_type(req, "text/plain");
    if (!s) return httpd_resp_sendstr(req, "no sensor");
    if (!s_af_loaded) return httpd_resp_sendstr(req, s_af_note);

    char a[12];
    if (!q_str(req, "a", a, sizeof(a))) return httpd_resp_sendstr(req, "a= required");

    int r = -1;
    if (!strcmp(a, "trigger"))     r = s->af_trigger  ? s->af_trigger(s)     : -1;
    else if (!strcmp(a, "auto"))   r = s->af_set_mode ? s->af_set_mode(s, 0) : -1;
    else if (!strcmp(a, "manual")) r = s->af_set_mode ? s->af_set_mode(s, 1) : -1;
    else return httpd_resp_sendstr(req, "a=trigger|auto|manual");

    /* Report the resulting status too — on a fixed-focus module the command
     * "succeeds" and the status still settles, which is exactly why this must
     * not be read as proof that the lens moved. */
    uint8_t raw = 0; bool foc = false, busy = false;
    if (s->af_get_status) s->af_get_status(s, &raw, &foc, &busy);
    char msg[140];
    snprintf(msg, sizeof(msg), "%s: %s - status 0x%02x (%s)%s", a,
             r == 0 ? "sent" : "FAILED", raw,
             foc ? "focused" : (busy ? "busy" : "idle"),
             r == 0 ? "" : " - command rejected");
    return httpd_resp_sendstr(req, msg);
}

/* ── WiFi scan / save ──────────────────────────────────────────────────────── */

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    httpd_resp_set_type(req, "application/json");
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return httpd_resp_sendstr(req, "[]");

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) return httpd_resp_sendstr(req, "[]");
    esp_wifi_scan_get_ap_records(&n, recs);

    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (uint16_t i = 0; i < n; i++) {
        /* Quotes/backslashes in an SSID would break the JSON — skip those rather
         * than ship a half-written escaper into a bringup tool. */
        if (strchr((char *) recs[i].ssid, '"') || strchr((char *) recs[i].ssid, '\\'))
            continue;
        char one[96];
        snprintf(one, sizeof(one), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 first ? "" : ",", (char *) recs[i].ssid, recs[i].rssi);
        httpd_resp_sendstr_chunk(req, one);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]");
    free(recs);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Tiny form-field reader: enough for ssid/pass. */
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

/* ── OTA ───────────────────────────────────────────────────────────────────── */

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
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
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

/* ── the tabbed UI ─────────────────────────────────────────────────────────── */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'>"
        "<title>BirdBox board test</title><style>"
        "body{background:#111;color:#eee;font:14px system-ui;margin:0;padding:10px;"
        "text-align:center}"
        "img{max-width:100%;border:1px solid #444;border-radius:6px}"
        "input,button,select{font:14px system-ui;padding:6px;margin:3px;"
        "border-radius:4px;border:1px solid #555;background:#222;color:#eee}"
        "button{cursor:pointer}button:hover{background:#2c2c2c}"
        ".tabs{margin:6px 0 12px}.tb{background:#1b1b1b}.tb.on{background:#2f4f6f;"
        "border-color:#6cf}.pane{display:none}.pane.on{display:block}"
        ".sh{color:#9cf;font-size:14px;margin:14px 0 4px}"
        ".sts{font-size:12px;color:#999;max-width:34em;margin:6px auto}"
        "#sts{min-height:1.2em;color:#9c9;font-size:13px;margin:6px}"
        ".row{margin:4px;display:flex;gap:8px;align-items:center;"
        "justify-content:center}.row label{width:118px;text-align:right;font-size:13px}"
        ".drow{display:flex;justify-content:space-between;max-width:30em;"
        "margin:0 auto;padding:2px 6px;border-bottom:1px solid #222;font-size:13px}"
        ".drow span:first-child{color:#aaa}.ok{color:#8d8}.bad{color:#e88}"
        "fieldset{border:1px solid #444;border-radius:6px;max-width:34em;"
        "margin:10px auto}legend{color:#9cf;font-size:13px}"
        "</style></head><body>"
        "<h2 style='margin:4px'>BirdBox board test</h2>"
        "<div class=tabs>"
        "<button class='tb on' id='t_live' onclick=\"tab('live')\">Live</button>"
        "<button class=tb id='t_set'  onclick=\"tab('set')\">Settings</button>"
        "<button class=tb id='t_dbg'  onclick=\"tab('dbg')\">Debug</button>"
        "<button class=tb id='t_wifi' onclick=\"tab('wifi')\">WiFi</button>"
        "<button class=tb id='t_ota'  onclick=\"tab('ota')\">OTA</button>"
        "</div><div id=sts></div>");

    /* Live */
    httpd_resp_sendstr_chunk(req,
        "<div class='pane on' id=livep>"
        "<img id=v alt='stream'>"
        "<fieldset><legend>Autofocus</legend>"
        "<div id=afn class=sts></div>"
        "<button onclick=\"af('trigger')\">Focus once</button>"
        "<button onclick=\"af('auto')\">Continuous</button>"
        "<button onclick=\"af('manual')\">Stop / manual</button>"
        "<div class=sts style='color:#c99'>A fixed-focus module accepts these and "
        "reports a status anyway &mdash; only a lens that visibly or audibly moves "
        "proves a VCM is fitted.</div></fieldset></div>");

    /* Settings */
    httpd_resp_sendstr_chunk(req,
        "<div class=pane id=setp>"
        "<div class=sh>Resolution</div>"
        "<select id=fs></select><button onclick='setfs()'>Apply</button>"
        "<div class=sts>No PSRAM, so large sizes may refuse to allocate &mdash; the "
        "board reverts to the previous size rather than losing the camera. Leaving the "
        "Live tab releases the stream, which re-init requires.</div>"
        "<div class=sh>Image</div><div id=ctls></div>"
        "<div class=sh>Exposure / gain / white balance</div><div id=ctls2></div>"
        "</div>");

    /* Debug */
    httpd_resp_sendstr_chunk(req,
        "<div class=pane id=dbgp>"
        "<div class=sh>Hardware</div><div id=dHw></div>"
        "<div class=sh>Memory</div><div id=dMem></div>"
        "<div class=sh>Camera</div><div id=dCam></div>"
        "<div class=sh>Network</div><div id=dNet></div>"
        "<div class=sh>Firmware</div><div id=dFw></div>"
        "<button onclick='loadDbg()'>&#8635; Refresh</button>"
        "<button onclick='rb()'>Reboot</button>"
        "<div class=sts>PSRAM is disabled in this build on purpose, so all figures "
        "here are internal DRAM. This is the tool for a board that cannot run BirdBox: "
        "if the stream works, PSRAM is the only defect.</div></div>");

    /* WiFi */
    httpd_resp_sendstr_chunk(req,
        "<div class=pane id=wifip>"
        "<div class=sh>Network</div><div id=wcur class=sts></div>"
        "<button onclick='scan()'>Scan networks</button><div id=nets></div>"
        "<form method='POST' action='/wifi-save'>"
        "<div><input name='ssid' id='ssid' placeholder='SSID' required></div>"
        "<div><input name='pass' type='password' placeholder='password'></div>"
        "<button type='submit'>Save &amp; reboot</button></form>"
        "<div class=sts>Saved to NVS, so it survives reflashing. To force SoftAP "
        "(BirdBox-Test / birdbox1234), reflash with an empty TEST_WIFI_SSID.</div></div>");

    /* OTA */
    httpd_resp_sendstr_chunk(req,
        "<div class=pane id=otap>"
        "<div class=sh>Firmware upload</div>"
        "<div class=sts>Rollback is enabled: an image that never reaches its "
        "&ldquo;mark valid&rdquo; call &mdash; which is what a PSRAM-enabled build does "
        "on this board, hanging in <code>cpu_start</code> &mdash; is reverted by the "
        "bootloader on the next boot. A failed PSRAM experiment costs a reboot, not a "
        "serial cable.</div>"
        "<div><input type='file' id='f' accept='.bin'></div>"
        "<button onclick='up()'>Upload &amp; reboot</button><div id='p'></div></div>");

    /* One script for everything, as in BirdBox. */
    httpd_resp_sendstr_chunk(req,
        "<script>"
        "var TABS=['live','set','dbg','wifi','ota'];"
        "var BLANK='data:image/gif;base64,R0lGODlhAQABAAAAACw=';"
        "var SZ=['QQVGA','QVGA','CIF','VGA','SVGA','XGA','HD','SXGA','UXGA','QXGA','QSXGA'];"
        "var A=[['brightness',-2,2],['contrast',-2,2],['saturation',-2,2],"
        "['sharpness',-2,2],['denoise',0,8],['quality',4,63],['special_effect',0,6],"
        "['hmirror',0,1],['vflip',0,1],['colorbar',0,1]];"
        "var B=[['aec',0,1],['aec2',0,1],['ae_level',-2,2],['aec_value',0,1200],"
        "['agc',0,1],['agc_gain',0,30],['awb',0,1],['awb_gain',0,1],['wb_mode',0,4],"
        "['bpc',0,1],['wpc',0,1],['raw_gma',0,1],['lenc',0,1],['dcw',0,1]];"
        "function $g(i){return document.getElementById(i);}"
        "function say(t){$g('sts').textContent=t;}"
        /* Dropping the img src releases the stream client, which is what lets the
         * Settings tab re-init the camera at a new resolution. */
        "function tab(n){TABS.forEach(function(x){"
        "$g(x+'p').className='pane'+(x===n?' on':'');"
        "$g('t_'+x).className='tb'+(x===n?' on':'');});"
        "var v=$g('v');if(n==='live'){v.src='/stream';}else{v.src=BLANK;}"
        "if(n==='dbg')loadDbg();if(n==='set')loadSet();if(n==='wifi')loadWifi();}"
        "function ctl(k,v){fetch('/api/ctl?k='+k+'&v='+v,{method:'POST'})"
        ".then(r=>r.text()).then(say).catch(function(){say('request failed');});}"
        "function af(a){fetch('/api/af?a='+a,{method:'POST'}).then(r=>r.text())"
        ".then(say).catch(function(){say('request failed');});}"
        "function rb(){if(!confirm('Reboot the board?'))return;"
        "fetch('/reboot',{method:'POST'}).then(function(){say('rebooting\\u2026');})"
        ".catch(function(){say('rebooting\\u2026');});}"
        "function setfs(){var v=$g('fs').value;say('re-initialising\\u2026');"
        "fetch('/api/ctl?k=framesize&v='+v,{method:'POST'}).then(r=>r.text())"
        ".then(function(t){say(t);loadSet();}).catch(function(){say('request failed');});}"
        "function mk(div,list,cur){var h='';list.forEach(function(c){"
        "var k=c[0],lo=c[1],hi=c[2],val=cur[k];"
        "h+=\"<div class=row><label>\"+k+\"</label>\";"
        "if(lo===0&&hi===1){h+=\"<input type=checkbox\"+(val?' checked':'')+"
        "\" onchange=\\\"ctl('\"+k+\"',this.checked?1:0)\\\">\";}"
        "else{h+=\"<input type=range min=\"+lo+\" max=\"+hi+\" value=\"+val+"
        "\" oninput=\\\"$g('n_\"+k+\"').textContent=this.value\\\"\"+"
        "\" onchange=\\\"ctl('\"+k+\"',this.value)\\\"><span id=n_\"+k+\">\"+val+\"</span>\";}"
        "h+='</div>';});div.innerHTML=h;}"
        "function afLine(d){return 'PID '+d.pid+' \\u2014 '+d.afNote+"
        "(d.afValid?(' \\u2014 status 0x'+d.afRaw.toString(16)+' '+"
        "(d.afFocused?'focused':(d.afBusy?'busy':'idle'))):'');}"
        "function loadSet(){fetch('/api/cam').then(r=>r.json()).then(function(d){"
        "var s=$g('fs');s.innerHTML='';"
        "SZ.forEach(function(n){var o=document.createElement('option');o.value=n;"
        "o.textContent=n+(n===d.framesize?' (current)':'');"
        "if(n===d.framesize)o.selected=true;s.appendChild(o);});"
        "mk($g('ctls'),A,d);mk($g('ctls2'),B,d);"
        "}).catch(function(){say('could not read /api/cam');});}"
        "function drow(k,v,c){return '<div class=drow><span>'+k+'</span><span'+"
        "(c?' class='+c:'')+'>'+v+'</span></div>';}"
        "function kb(n){return Math.round(n/1024)+' KB';}"
        "function loadDbg(){fetch('/api/sysinfo').then(r=>r.json()).then(function(d){"
        "$g('dHw').innerHTML=drow('Chip',d.chipModel+' rev '+d.chipRev+', '+"
        "d.chipCores+' cores @ '+d.cpuMhz+' MHz')+"
        "drow('Flash',d.flashMB+' MB (id '+d.flashId+')')+"
        "drow('PSRAM',d.psram)+"
        "drow('Emb-PSRAM bit',(d.embPsramBit?'yes':'no')+' \\u2014 unreliable, "
        "disagrees with esptool');"
        "$g('dMem').innerHTML=drow('Free DRAM',kb(d.dram))+"
        "drow('Largest block',kb(d.dramBig),d.dramBig<40960?'bad':'ok');"
        "$g('dCam').innerHTML=drow('Sensor PID',d.sensorPid)+"
        "drow('Resolution',d.framesize)+drow('Autofocus',d.afNote)+"
        "drow('Frames sent',d.frames)+"
        "drow('fb failures',d.fbErrors,d.fbErrors?'bad':'ok')+"
        "drow('Stream clients',d.streamClients);"
        "$g('dNet').innerHTML=drow('Mode',d.mode)+drow('IP',d.ip)+"
        "drow('SSID',d.ssid||'\\u2014')+drow('RSSI',d.rssi+' dBm');"
        "$g('dFw').innerHTML=drow('OTA slot',d.otaSlot)+drow('IDF',d.idfVer)+"
        "drow('Built',d.buildDate)+drow('Uptime',d.uptime+' s');"
        "}).catch(function(){say('could not read /api/sysinfo');});}"
        "function loadWifi(){fetch('/api/sysinfo').then(r=>r.json()).then(function(d){"
        "$g('wcur').textContent='Mode '+d.mode+', IP '+d.ip+', saved SSID '+"
        "(d.ssid||'(none)');}).catch(function(){});}"
        "function scan(){var d=$g('nets');d.textContent='scanning\\u2026';"
        "fetch('/api/scan').then(r=>r.json()).then(function(a){var h='';"
        "a.forEach(function(n){h+=\"<div class=drow><span><a href='#' onclick=\\\""
        "$g('ssid').value='\"+n.ssid+\"';return false\\\" style='color:#6cf'>\"+"
        "n.ssid+\"</a></span><span>\"+n.rssi+\" dBm</span></div>\";});"
        "d.innerHTML=h||'none found';}).catch(function(){d.textContent='scan failed';});}"
        "function up(){var f=$g('f').files[0];var p=$g('p');"
        "if(!f){p.textContent='pick a .bin first';return;}"
        "var x=new XMLHttpRequest();x.open('POST','/ota/upload',true);"
        "x.setRequestHeader('Content-Type','application/octet-stream');"
        "x.upload.onprogress=function(e){if(e.lengthComputable)"
        "p.textContent='uploading '+Math.round(e.loaded/e.total*100)+'%';};"
        "x.onload=function(){p.textContent=x.status===200?'ok \\u2014 rebooting':"
        "('error: '+x.responseText);};"
        "x.onerror=function(){p.textContent='upload error';};x.send(f);}"
        /* Live AF status poll, only while the Live tab is showing. */
        "setInterval(function(){if($g('livep').className.indexOf('on')<0)return;"
        "fetch('/api/cam').then(r=>r.json()).then(function(d){"
        "$g('afn').textContent=afLine(d);}).catch(function(){});},2000);"
        "$g('v').src='/stream';"
        "fetch('/api/cam').then(r=>r.json()).then(function(d){"
        "$g('afn').textContent=afLine(d);}).catch(function(){});"
        "</script></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ── HTTP server ───────────────────────────────────────────────────────────── */

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 6144;
    cfg.lru_purge_enable = true;
    /* Registrations past the cap are silently dropped and the route just 404s
     * (a BirdBox lesson), so keep real headroom over the 11 below. */
    cfg.max_uri_handlers = 24;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }

    const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_index      },
        { .uri = "/stream",      .method = HTTP_GET,  .handler = h_stream     },
        { .uri = "/health",      .method = HTTP_GET,  .handler = h_health     },
        { .uri = "/api/sysinfo", .method = HTTP_GET,  .handler = h_sysinfo    },
        { .uri = "/api/cam",     .method = HTTP_GET,  .handler = h_cam_json   },
        { .uri = "/api/ctl",     .method = HTTP_POST, .handler = h_ctl        },
        { .uri = "/api/af",      .method = HTTP_POST, .handler = h_af         },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = h_scan       },
        { .uri = "/wifi-save",   .method = HTTP_POST, .handler = h_wifi_save  },
        { .uri = "/ota/upload",  .method = HTTP_POST, .handler = h_ota_upload },
        { .uri = "/reboot",      .method = HTTP_POST, .handler = h_reboot     },
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
 * so the WiFi tab's network scan still works. */
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

    ESP_LOGI(TAG, "streamtest — PSRAM-free board bringup (tabs, OTA, WiFi, AF)");
    ESP_LOGI(TAG, "free DRAM at boot: %u B",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Camera first: if it cannot allocate its buffer, that is the headline
     * result. We do NOT return early — the network still comes up so the board
     * stays reachable for OTA even with a dead sensor. */
    bool cam_ok = (camera_start(s_fs) == ESP_OK);
    if (cam_ok) af_setup();
    else ESP_LOGE(TAG, "camera init FAILED — /stream will not work, "
                       "bringing up network anyway for OTA");

    wifi_start();
    http_start();

    /* Rollback gate. Only confirm this image once the pieces that matter are
     * running, so a broken build gets reverted instead of sticking. The camera
     * is deliberately NOT required: a board with a dead sensor must still keep
     * OTA reachable, or it becomes unrecoverable without serial. */
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
