/* Embedded web UI + REST API (FSD §5, §6).
 *
 * Implemented so far: the WiFi config portal (setup page, /api/scan,
 * /wifi-save — FSD §4), a placeholder dashboard, /api/status and
 * /api/reboot. Remaining tabs (Live/Gallery/Stats/Settings/Debug/OTA)
 * arrive with their subsystems.
 *
 * httpd config carries the RemoteStart lessons: lru_purge_enable (v1.35),
 * handler-count headroom + loud registration failures (v1.27). */
#include "web_server.h"
#include "version.h"
#include "wifi.h"
#include "camera.h"
#include "storage.h"
#include "motion.h"
#include "capture.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "web";

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ── WiFi setup page (portal + later the WiFi tab) ──────────────────────── */
static const char WIFI_SETUP_HTML[] =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>BirdBox WiFi Setup</title>"
"<style>"
"body{font-family:Arial,sans-serif;background:#16281c;color:#eee;display:flex;"
"justify-content:center;align-items:center;min-height:100vh;margin:0}"
".box{background:#1e3826;border-radius:10px;padding:24px;width:100%;max-width:340px}"
"h2{color:#7fc98b;margin:0 0 14px;font-size:1.2rem}"
"label{display:block;margin:10px 0 4px;font-size:.85rem}"
"input{width:100%;padding:9px;background:#16281c;border:1px solid #444;"
"border-radius:5px;color:#eee;box-sizing:border-box}"
".btn{width:100%;padding:10px;border:none;border-radius:5px;color:#fff;"
"cursor:pointer;font-size:.95rem;margin-top:12px}"
".btn-s{background:#2a4d34;border:1px solid #7fc98b}"
".btn-c{background:#3f8a4f}"
".sts{font-size:.75rem;color:#9ab;min-height:1rem;margin:8px 0 4px}"
".nets{max-height:180px;overflow-y:auto;margin-top:6px}"
".net{background:#2a4d34;border-radius:5px;padding:8px 12px;margin-bottom:4px;"
"cursor:pointer;display:flex;justify-content:space-between;align-items:center;font-size:.85rem}"
".net:hover{background:#356343}"
".meta{font-size:.72rem;color:#aac}"
".lk{color:#7fc98b}"
"</style></head>"
"<body><div class='box'>"
"<h2>&#128038; " FIRMWARE_NAME " WiFi Setup</h2>"
"<button class='btn btn-s' onclick='doScan()'>&#128246; Scan Networks</button>"
"<div class='sts' id='sts'></div>"
"<div class='nets' id='nets'></div>"
"<form action='/wifi-save' method='POST'>"
"<label>SSID</label>"
"<input id='sid' name='ssid' type='text' placeholder='tap network or type' required>"
"<label>Password</label>"
"<input name='pass' type='password' placeholder='leave blank if open'>"
"<button class='btn btn-c' type='submit'>Connect &amp; Save</button>"
"</form>"
"<script>"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
"function doScan(){"
"var st=document.getElementById('sts');"
"st.textContent='Scanning\\u2026';"
"document.getElementById('nets').innerHTML='';"
"fetch('/api/scan').then(function(r){return r.json();})"
".then(function(a){"
"st.textContent=a.length+' network'+(a.length!==1?'s':'')+' found';"
"var c=document.getElementById('nets');"
"a.forEach(function(n){"
"var d=document.createElement('div');d.className='net';"
"d.innerHTML='<span>'+esc(n.ssid)+'</span>"
"<span class=meta>'+n.rssi+'dBm "
"'+(n.auth?'<span class=lk>&#128274;</span>':'open')+'</span>';"
"d.onclick=function(){document.getElementById('sid').value=n.ssid;};"
"c.appendChild(d);});}).catch(function(){"
"st.textContent='Scan failed \\u2014 try again';});}"
"</script>"
"</div></body></html>";

/* Tabbed dashboard (FSD §5): Live + Gallery implemented; Stats/Settings/
 * Debug/WiFi/OTA tabs land with their subsystems. */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>BirdBox</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:Arial,sans-serif;background:#16281c;color:#eee;margin:0}"
".hdr{background:#1e3826;padding:12px 20px;display:flex;align-items:center;gap:14px;"
"border-bottom:2px solid #7fc98b}"
".hdr h1{font-size:1.1rem;color:#7fc98b;margin:0}.hdr .v{font-size:.75rem;color:#9ab}"
".tabs{display:flex;background:#1e3826}"
".tab{padding:10px 20px;cursor:pointer;color:#aac;border:none;background:none;"
"font-size:.9rem;border-bottom:3px solid transparent}"
".tab.on{color:#7fc98b;border-bottom-color:#7fc98b;font-weight:bold}"
".pane{display:none;padding:16px;max-width:960px;margin:0 auto}.pane.on{display:block}"
".live{width:100%;border-radius:8px;background:#000;min-height:200px}"
".sts{font-size:.78rem;color:#9ab;margin-top:8px}"
"a{color:#8fd39b}"
"button.act{padding:8px 16px;border:none;border-radius:5px;background:#3f8a4f;"
"color:#fff;cursor:pointer;font-size:.85rem;margin:8px 8px 0 0}"
"select{background:#1e3826;color:#eee;border:1px solid #444;border-radius:5px;"
"padding:7px;font-size:.85rem}"
".gbar{display:flex;gap:10px;align-items:center;margin-bottom:12px}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}"
".gitem{background:#1e3826;border-radius:8px;overflow:hidden}"
".gitem img{width:100%;display:block;aspect-ratio:4/3;object-fit:cover;background:#000}"
".gmeta{display:flex;justify-content:space-between;align-items:center;"
"padding:6px 8px;font-size:.72rem;color:#9ab}"
".gmeta button{background:none;border:none;color:#e77;cursor:pointer;font-size:.9rem}"
"</style></head><body>"
"<div class='hdr'><h1>&#128038; " FIRMWARE_NAME "</h1>"
"<span class='v'>v" FIRMWARE_VERSION "</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"livep\",this)'>Live</button>"
"<button class='tab' onclick='show(\"galleryp\",this)'>Gallery</button>"
"<button class='tab' onclick='location.href=\"/wifi-setup\"'>WiFi</button>"
"</div>"
"<div id='livep' class='pane on'>"
"<img class='live' id='live' src='/stream' alt='live stream'"
" onerror=\"this.alt='no camera / stream unavailable';\">"
"<div class='sts' id='sts'></div>"
"<button class='act' onclick='snap()'>&#128247; Snapshot to SD</button>"
"</div>"
"<div id='galleryp' class='pane'>"
"<div class='gbar'><select id='day' onchange='loadDay()'></select>"
"<button class='act' style='margin:0' onclick='loadDays()'>&#8635; Refresh</button>"
"<span class='sts' id='gsts' style='margin:0'></span></div>"
"<div class='grid' id='grid'></div>"
"</div>"
"<script>"
"function show(id,btn){"
"document.querySelectorAll('.pane').forEach(e=>e.classList.remove('on'));"
"document.querySelectorAll('.tab').forEach(e=>e.classList.remove('on'));"
"document.getElementById(id).classList.add('on');btn.classList.add('on');"
"var lv=document.getElementById('live');"
"if(id==='livep'){if(!lv.src.endsWith('/stream'))lv.src='/stream';}"
"else{lv.src='';}"   /* stop streaming while hidden — frees a stream slot */
"if(id==='galleryp')loadDays();}"
"function tick(){fetch('/api/status').then(r=>r.json()).then(s=>{"
"var t=s.ip+' | RSSI '+s.rssi+' dBm | heap '+Math.round(s.heap/1024)+' KB | up '+s.uptime+' s'"
"+' | SD '+(s.sdPresent?s.sdFreeMB+' MB free':'none')"
"+' | motion '+(s.motion?'ACTIVE':'idle')+' | events '+s.events;"
"if(s.lastEvent)t+=' | last: <a href=\"'+s.lastEvent+'\">'+s.lastEvent.split('/').pop()+'</a>';"
"document.getElementById('sts').innerHTML=t;"
"}).catch(()=>{});}tick();setInterval(tick,2000);"
"function snap(){fetch('/api/capture',{method:'POST'}).then(r=>r.json())"
".then(o=>{alert(o.path?('Saved '+o.path):JSON.stringify(o));}).catch(()=>alert('failed'));}"
"function loadDays(){fetch('/api/days').then(r=>r.json()).then(a=>{"
"a.sort((x,y)=>y.d.localeCompare(x.d));"
"var s=document.getElementById('day');var prev=s.value;s.innerHTML='';"
"a.forEach(o=>{var op=document.createElement('option');op.value=o.d;"
"op.textContent=o.d+' ('+o.n+')';s.appendChild(op);});"
"if(prev&&[...s.options].some(o=>o.value===prev))s.value=prev;"
"if(a.length)loadDay();else{document.getElementById('grid').innerHTML='';"
"document.getElementById('gsts').textContent='no captures yet';}});}"
"function loadDay(){var d=document.getElementById('day').value;if(!d)return;"
"fetch('/api/events?date='+d).then(r=>r.json()).then(a=>{"
"a.sort((x,y)=>y.f.localeCompare(x.f));"
"document.getElementById('gsts').textContent=a.length+' capture'+(a.length!==1?'s':'');"
"var g=document.getElementById('grid');g.innerHTML='';"
"a.forEach(o=>{var p='/captures/'+d+'/'+o.f;"
"var div=document.createElement('div');div.className='gitem';"
"div.innerHTML='<a href=\"'+p+'\" target=\"_blank\">"
"<img loading=\"lazy\" src=\"'+p+'\"></a>"
"<div class=\"gmeta\"><span>'+o.f+' &middot; '+Math.round(o.s/1024)+' KB</span>"
"<button title=\"delete\" onclick=\"del(\\''+p+'\\')\">&#10060;</button></div>';"
"g.appendChild(div);});});}"
"function del(p){if(!confirm('Delete '+p.split('/').pop()+'?'))return;"
"fetch(p,{method:'DELETE'}).then(()=>loadDays());}"
"</script></body></html>";

/* ── MJPEG live stream (FSD §3.3) ───────────────────────────────────────────
 * multipart/x-mixed-replace, one JPEG per part. ESP-IDF's httpd is single-
 * threaded, so an endless synchronous handler would block every other
 * request; each stream client instead runs in its own task on an async
 * request copy (httpd_req_async_handler_begin), keeping the UI responsive.
 * Max 2 concurrent clients (memory bound, §3.3) — further clients get 503. */
#define STREAM_MAX_CLIENTS  2
#define STREAM_BOUNDARY     "\r\n--frame\r\n"
#define STREAM_PART_HDR     "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static volatile int s_stream_clients = 0;

static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *) arg;
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");

    char hdr[64];
    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "stream: no frame from camera");
            break;
        }
        int hlen = snprintf(hdr, sizeof(hdr), STREAM_PART_HDR, (unsigned) fb->len);
        bool fail =
            httpd_resp_send_chunk(req, STREAM_BOUNDARY, sizeof(STREAM_BOUNDARY) - 1) != ESP_OK ||
            httpd_resp_send_chunk(req, hdr, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *) fb->buf, fb->len) != ESP_OK;
        esp_camera_fb_return(fb);
        if (fail) break;               /* client went away */
        vTaskDelay(pdMS_TO_TICKS(30)); /* pace ≈ sensor rate, yield to httpd */
    }

    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    s_stream_clients--;
    ESP_LOGI(TAG, "stream client disconnected (%d active)", s_stream_clients);
    vTaskDelete(NULL);
}

static esp_err_t h_stream(httpd_req_t *req)
{
    if (!camera_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "no camera detected");
        return ESP_OK;
    }
    if (s_stream_clients >= STREAM_MAX_CLIENTS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "stream busy: max 2 concurrent clients");
        return ESP_OK;
    }

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
        return ESP_OK;
    }
    s_stream_clients++;
    if (xTaskCreate(stream_task, "stream", 6144, copy, 5, NULL) != pdPASS) {
        s_stream_clients--;
        httpd_req_async_handler_complete(copy);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory for stream task");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "stream client connected (%d active)", s_stream_clients);
    return ESP_OK;
}

/* ── Handlers ───────────────────────────────────────────────────────────── */
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* Portal mode: whatever the user opens lands on the setup page */
    httpd_resp_sendstr(req, wifi_in_portal_mode() ? WIFI_SETUP_HTML : INDEX_HTML);
    return ESP_OK;
}

static esp_err_t h_wifi_setup(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, WIFI_SETUP_HTML);
    return ESP_OK;
}

/* In-place decode of application/x-www-form-urlencoded text: '+' becomes a
 * space and %XX becomes the encoded byte. Without this, any SSID or password
 * containing a space or symbol is saved in encoded form and the device
 * reboots into a connect-fail/portal loop (RemoteStart v1.29 lesson). */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        int hi, lo;
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0) {
            *o++ = (char) (hi * 16 + lo);
            s += 3;
        } else *o++ = *s++;
    }
    *o = '\0';
}

/* POST /wifi-save — credentials from the portal form / WiFi tab */
static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char body[256] = {0};
    int  len = MIN(req->content_len, (int) sizeof(body) - 1);
    httpd_req_recv(req, body, len);

    char *sp = strstr(body, "ssid=");
    char *pp = strstr(body, "pass=");
    if (sp) {
        sp += 5;
        char *end = strchr(sp, '&');
        if (end) *end = '\0';
        strlcpy(g_new_ssid, sp, sizeof(g_new_ssid));
        if (end) *end = '&';
    }
    if (pp) {
        pp += 5;
        char *end = strchr(pp, '&');
        if (end) *end = '\0';
        strlcpy(g_new_pass, pp, sizeof(g_new_pass));
    }
    url_decode(g_new_ssid);
    url_decode(g_new_pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#16281c;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#7fc98b'>Saved!</h2>"
        "<p>Connecting to WiFi and rebooting…</p></div></body></html>");

    g_wifi_save_requested = true;
    return ESP_OK;
}

/* GET /api/scan — WiFi scan, JSON array (works in STA and APSTA/portal) */
static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t scfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scfg, true);

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) count = 20;

    wifi_ap_record_t *recs = count ? malloc(count * sizeof(wifi_ap_record_t)) : NULL;
    if (recs) esp_wifi_scan_get_ap_records(&count, recs);
    else count = 0;

    char *buf = malloc((int) count * 140 + 8);
    if (!buf) {
        free(recs);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    strcpy(buf, "[");
    bool first = true;
    for (int i = 0; i < (int) count; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        char esc[70] = {0};
        int j = 0;
        for (int k = 0; recs[i].ssid[k] && k < 32 && j < 66; k++) {
            unsigned char c = recs[i].ssid[k];
            if (c == '"' || c == '\\') esc[j++] = '\\';
            if (c >= 0x20) esc[j++] = c;
        }
        char entry[140];
        snprintf(entry, sizeof(entry), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 first ? "" : ",", esc, recs[i].rssi, (int) recs[i].authmode);
        strcat(buf, entry);
        first = false;
    }
    strcat(buf, "]");
    free(recs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

/* POST /api/capture — manual snapshot to SD (FSD §6) */
static esp_err_t h_capture(httpd_req_t *req)
{
    if (!camera_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"no camera\"}");
        return ESP_OK;
    }
    if (!storage_sd_present()) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "{\"error\":\"no SD card\"}");
        return ESP_OK;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
        return ESP_OK;
    }
    char path[96] = {0};
    esp_err_t err = storage_save_jpeg(fb->buf, fb->len, path, sizeof(path));
    unsigned bytes = fb->len;
    esp_camera_fb_return(fb);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed");
        return ESP_OK;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"path\":\"%s\",\"bytes\":%u}", path, bytes);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /captures/... — static serving of stored JPEGs from SD (FSD §6) */
static esp_err_t h_captures_file(httpd_req_t *req)
{
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD card");
        return ESP_OK;
    }
    if (strstr(req->uri, "..") || strlen(req->uri) > 120) {   /* no traversal out of /sd */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    char path[160];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "%.120s", req->uri);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/days — capture day-folders with file counts (Gallery tab).
 * Chunked JSON so a card full of days never needs one big buffer. */
static esp_err_t h_days(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    DIR *d = storage_sd_present() ? opendir(STORAGE_MOUNT_POINT "/captures") : NULL;
    if (!d) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *e;
    bool first = true;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR || e->d_name[0] == '.') continue;
        char sub[112];
        snprintf(sub, sizeof(sub), STORAGE_MOUNT_POINT "/captures/%.64s", e->d_name);
        int n = 0;
        DIR *d2 = opendir(sub);
        if (d2) {
            struct dirent *e2;
            while ((e2 = readdir(d2)) != NULL)
                if (e2->d_type == DT_REG) n++;
            closedir(d2);
        }
        char item[112];
        int len = snprintf(item, sizeof(item), "%s{\"d\":\"%.64s\",\"n\":%d}",
                           first ? "" : ",", e->d_name, n);
        httpd_resp_send_chunk(req, item, len);
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/events?date=YYYY-MM-DD — files of one capture day (Gallery tab) */
static esp_err_t h_events(httpd_req_t *req)
{
    char query[64] = {0}, date[36] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "date", date, sizeof(date));
    for (const char *c = date; *c; c++) {            /* dirname chars only */
        if (!isalnum((unsigned char) *c) && *c != '-') {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad date");
            return ESP_OK;
        }
    }

    httpd_resp_set_type(req, "application/json");
    char dir[112];
    snprintf(dir, sizeof(dir), STORAGE_MOUNT_POINT "/captures/%.36s", date);
    DIR *d = (storage_sd_present() && date[0]) ? opendir(dir) : NULL;
    if (!d) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *e;
    bool first = true;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG || e->d_name[0] == '.') continue;
        char fpath[176];
        snprintf(fpath, sizeof(fpath), "%s/%.48s", dir, e->d_name);
        struct stat st = {0};
        stat(fpath, &st);
        char item[128];
        int len = snprintf(item, sizeof(item), "%s{\"f\":\"%.48s\",\"s\":%ld}",
                           first ? "" : ",", e->d_name, (long) st.st_size);
        httpd_resp_send_chunk(req, item, len);
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* DELETE /captures/... — remove one stored capture (Gallery tab) */
static esp_err_t h_captures_delete(httpd_req_t *req)
{
    if (!storage_sd_present() || strstr(req->uri, "..") || strlen(req->uri) > 120) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    char path[160];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "%.120s", req->uri);
    if (unlink(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/status — minimal for now, grows with the subsystems (FSD §6) */
static esp_err_t h_status(httpd_req_t *req)
{
    char ip[16] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));

    wifi_ap_record_t ap = {0};
    int rssi = 0, ch = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { rssi = ap.rssi; ch = ap.primary; }

    uint64_t sd_total, sd_free;
    storage_get_info(&sd_total, &sd_free);

    char buf[448];
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"version\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"heap\":%lu,\"uptime\":%lld,\"portal\":%s,\"wifiReconnects\":%lu,"
        "\"sdPresent\":%s,\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,"
        "\"motion\":%s,\"events\":%lu,\"lastEvent\":\"%s\"}",
        FIRMWARE_NAME, FIRMWARE_VERSION, ip, rssi, ch,
        (unsigned long) esp_get_free_heap_size(),
        esp_timer_get_time() / 1000000,
        wifi_in_portal_mode() ? "true" : "false",
        (unsigned long) g_wifi_disconnect_count,
        storage_sd_present() ? "true" : "false",
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        motion_active() ? "true" : "false",
        (unsigned long) capture_event_count(),
        capture_last_event_path());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/reboot */
static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "Reboot requested via API");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── Start Web Server ───────────────────────────────────────────────────── */
esp_err_t web_server_start(void)
{
    static httpd_handle_t server = NULL;
    if (server) return ESP_OK;   /* already running (portal path starts us early) */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;     /* headroom above route count — an exact-fit cap
                                      silently drops later registrations (RemoteStart v1.27) */
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;   /* abandoned sessions must not exhaust the socket
                                      pool and wedge the server (RemoteStart v1.35) */
    cfg.uri_match_fn     = httpd_uri_match_wildcard;   /* for the /captures wildcard route */

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root       },
        { .uri = "/stream",      .method = HTTP_GET,  .handler = h_stream     },
        { .uri = "/wifi-setup",  .method = HTTP_GET,  .handler = h_wifi_setup },
        { .uri = "/wifi-save",   .method = HTTP_POST, .handler = h_wifi_save  },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = h_scan       },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = h_status     },
        { .uri = "/api/capture", .method = HTTP_POST, .handler = h_capture    },
        { .uri = "/api/days",    .method = HTTP_GET,  .handler = h_days       },
        { .uri = "/api/events",  .method = HTTP_GET,  .handler = h_events     },
        { .uri = "/captures/*",  .method = HTTP_GET,  .handler = h_captures_file },
        { .uri = "/captures/*",  .method = HTTP_DELETE, .handler = h_captures_delete },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = h_reboot     },
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Failed to register %s: %s", routes[i].uri, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Web server started (%u routes)",
             (unsigned) (sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
