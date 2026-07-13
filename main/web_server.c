/* Embedded web UI + REST API (FSD §5, §6). All tabs implemented: Live,
 * Gallery, Stats, Settings, Debug, WiFi, OTA Update.
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
#include "stats.h"
#include "settings.h"
#include "classify.h"
#include "species_i18n.h"
#include "board_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
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
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/temperature_sensor.h"
#include "driver/gpio.h"
#include "illum.h"

#include <sys/socket.h>   /* recv() peek — detect a stream client's TCP close */
#include <sys/time.h>     /* struct timeval for SO_SNDTIMEO */
#include <errno.h>

static const char *TAG = "web";

/* Clock source for the time shown in the UI (FSD §3.4): "ntp" once SNTP has
 * synced, "manual" when only the browser-time fallback (POST /api/time) set it,
 * "none" before either. */
static bool s_clock_manual = false;

void web_server_note_ntp_sync(void)
{
    if (s_clock_manual) {
        s_clock_manual = false;
        ESP_LOGI(TAG, "SNTP sync landed — clockSrc back to ntp");
    }
}

static void device_time(char *out, size_t n, const char **src)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2020) { if (n) out[0] = '\0'; *src = "none"; return; }
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
    *src = s_clock_manual ? "manual" : "ntp";
}

/* On-die temperature sensor for the Debug tab / overheat diagnosis (FSD §5).
 * Installed lazily on first /api/sysinfo read. This is the SoC die, not the
 * camera, but it's a solid proxy for whether the box is running hot. Returns
 * -1000 if unavailable. */
static temperature_sensor_handle_t s_tsens;
static float soc_temp_c(void)
{
    if (!s_tsens) {
        /* 20–100 °C: must be a single predefined S3 range (a wider span makes
         * install fail). A running box sits well above 20 °C, so this covers
         * the overheat zone we care about. */
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
        esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
        if (err != ESP_OK) {
            s_tsens = NULL;
            ESP_LOGW(TAG, "temp sensor install failed: %s", esp_err_to_name(err));
            return -1000.0f;
        }
        temperature_sensor_enable(s_tsens);
    }
    float c = -1000.0f;
    esp_err_t err = temperature_sensor_get_celsius(s_tsens, &c);
    if (err != ESP_OK) { ESP_LOGW(TAG, "temp read failed: %s", esp_err_to_name(err)); return -1000.0f; }
    return c;
}

/* Heap low-water mark, tracked by main.c's housekeeping task (FSD §5) */
extern uint32_t g_heap_min;
extern int64_t  g_heap_min_ts_us;

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
".note{font-size:.72rem;color:#9ab;margin-top:8px}"
".iplink{display:inline-block;font-size:1.05rem;color:#7fc98b;"
"word-break:break-all;margin:6px 0}"
"</style></head>"
"<body><div class='box'>"
"<h2>&#128038; " FIRMWARE_NAME " WiFi Setup</h2>"
"<button class='btn btn-s' onclick='doScan()'>&#128246; Scan Networks</button>"
"<div class='sts' id='sts'></div>"
"<div class='nets' id='nets'></div>"
"<form id='wf' onsubmit='return doSave(event)'>"
"<label>SSID</label>"
"<input id='sid' name='ssid' type='text' placeholder='tap network or type' required>"
"<label>Password</label>"
"<input id='pw' name='pass' type='password' placeholder='leave blank if open'>"
"<div class='note'>&#8505;&#65039; IP is assigned automatically (DHCP).</div>"
"<button class='btn btn-c' type='submit'>Connect &amp; Save</button>"
"</form>"
"<div id='res' style='display:none'></div>"
"<script>"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
"function doSave(e){e.preventDefault();"
"var sid=document.getElementById('sid').value;"
"if(!sid){return false;}"
"var pw=document.getElementById('pw').value;"
"document.getElementById('wf').style.display='none';"
"var r=document.getElementById('res');r.style.display='block';"
"r.innerHTML='<div class=\"sts\">Connecting to '+esc(sid)+'\\u2026 this can take ~20s.</div>'+"
"'<p class=\"note\">Your phone may briefly drop off <b>BirdBox-Config</b> while "
"the box joins the network \\u2014 if it does, just reconnect to "
"<b>BirdBox-Config</b> and the address will appear here.</p>';"
"var body='ssid='+encodeURIComponent(sid)+'&pass='+encodeURIComponent(pw);"
"fetch('/wifi-save',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
"setTimeout(poll,2000);return false;}"
"function poll(){"
"fetch('/api/portal-status').then(function(r){return r.json();}).then(function(s){"
"var r=document.getElementById('res');"
"if(s.state==='connected'){done(s.ip);}"
"else if(s.state==='failed'){"
"r.innerHTML='<div class=\"sts\" style=\"color:#f88\">&#10060; Couldn&#39;t connect \\u2014 "
"check the password and try again.</div>"
"<button class=\"btn btn-c\" onclick=\"location.reload()\">Try again</button>';}"
"else{setTimeout(poll,1500);}"
"}).catch(function(){setTimeout(poll,1500);});}"
"function done(ip){var n=10;"
"function tick(){"
"document.getElementById('res').innerHTML="
"'<div class=\"sts\" style=\"color:#7fc98b\">&#9989; Connected!</div>'+"
"'<p style=\"font-size:.9rem\">Your BirdBox is at</p>'+"
"'<a class=\"iplink\" href=\"http://'+ip+'\">http://'+ip+'</a>'+"
"'<p class=\"note\">Reconnect your phone to your home WiFi, then open that "
"address. Rebooting in '+n+'s\\u2026</p>';"
"if(n<=0){return;}n--;setTimeout(tick,1000);}"
"tick();}"
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

/* Tabbed dashboard (FSD §5): Live/Gallery/Stats/Settings/WiFi implemented;
 * Debug and OTA tabs land with their subsystems. */
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
".liveWrap{max-width:960px;margin:0 auto;position:relative;overflow:hidden;"
"border-radius:8px;background:#000;padding-top:75%}"
".liveWrap.r1,.liveWrap.r3{padding-top:133.33%}"
".live{position:absolute;top:50%;left:50%;width:100%;transform:translate(-50%,-50%)}"
".live.r1{width:133.33%;transform:translate(-50%,-50%) rotate(90deg)}"
".live.r3{width:133.33%;transform:translate(-50%,-50%) rotate(270deg)}"
".rotbar{display:flex;align-items:center;gap:8px;margin-bottom:8px}"
".detbadge{position:absolute;top:8px;left:8px;z-index:3;display:none;"
"align-items:center;gap:6px;background:rgba(200,40,40,.9);color:#fff;"
"font-size:.72rem;font-weight:700;letter-spacing:.05em;padding:4px 9px;"
"border-radius:4px}"
".detbadge.on{display:inline-flex}"
".detbadge .dot{width:9px;height:9px;border-radius:50%;background:#fff;"
"animation:detpulse 1s ease-in-out infinite}"
".liveWrap.det{outline:3px solid rgba(220,60,60,.9);outline-offset:-3px}"
".liveWrap.mot{outline:5px solid #ff2f2f;outline-offset:-5px;"   /* 1s per-trigger motion flash */
"box-shadow:inset 0 0 18px rgba(255,47,47,.6)}"
"@keyframes detpulse{0%,100%{opacity:1}50%{opacity:.25}}"
".pausebadge{position:absolute;top:8px;right:8px;z-index:3;display:none;"
"align-items:center;gap:6px;background:rgba(180,140,40,.92);color:#1a1205;"
"font-size:.72rem;font-weight:700;letter-spacing:.05em;padding:4px 9px;"
"border-radius:4px}"
".livemsg{position:absolute;inset:0;z-index:4;display:none;flex-direction:column;"
"align-items:center;justify-content:center;gap:10px;text-align:center;padding:18px;"
"background:rgba(10,20,14,.9);color:#dfe8e2;font-size:.95rem}"
".livemsg.on{display:flex}"
".livemsg .lms{font-size:.8rem;color:#9fb0a6;max-width:280px}"
".livemsg button{background:#2f6f45;color:#eaf6ee;border:none;border-radius:6px;"
"padding:6px 16px;font-size:.85rem;cursor:pointer}"
".pausebadge.on{display:inline-flex}"
"button.act.off{background:#8a6d3f}"
".zone{position:absolute;inset:0;z-index:4;display:none;"
"grid-template-columns:repeat(8,1fr);grid-template-rows:repeat(8,1fr)}"
".zone.on{display:grid}"
".zcell{border:1px solid rgba(255,255,255,.28);cursor:pointer}"
".zcell.off{background:rgba(200,40,40,.5)}"
".zcell.inz{background:rgba(80,210,110,.10)}"
".motgrid{position:absolute;inset:0;z-index:2;display:grid;pointer-events:none;"   /* live trigger-cell overlay */
"grid-template-columns:repeat(8,1fr);grid-template-rows:repeat(8,1fr)}"
".mcell{}"
".mcell.hit{background:rgba(255,47,47,.38);outline:1.5px solid rgba(255,90,90,.95);outline-offset:-1.5px}"
".zbar{margin-top:8px;display:none;flex-wrap:wrap;gap:6px;align-items:center}"
".zbar.on{display:flex}"
".sts{font-size:.78rem;color:#9ab;margin-top:8px}"
"a{color:#8fd39b}"
"button.act{padding:8px 16px;border:none;border-radius:5px;background:#3f8a4f;"
"color:#fff;cursor:pointer;font-size:.85rem;margin:8px 8px 0 0}"
"select{background:#1e3826;color:#eee;border:1px solid #444;border-radius:5px;"
"padding:7px;font-size:.85rem}"
".gbar{display:flex;gap:10px;align-items:center;margin-bottom:12px}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}"
".gitem{background:#1e3826;border-radius:8px;overflow:hidden;position:relative}"
".gitem img{width:100%;display:block;aspect-ratio:4/3;object-fit:cover;background:#000}"
".gitem.sel{outline:3px solid #7fc98b;outline-offset:-3px}"
".glabel{position:absolute;top:6px;left:6px;background:rgba(20,40,28,.82);color:#7fc98b;"
"font-size:.66rem;padding:2px 6px;border-radius:4px;max-width:82%;white-space:nowrap;"
"overflow:hidden;text-overflow:ellipsis}"
".gchk{position:absolute;top:6px;right:6px;width:20px;height:20px;cursor:pointer;z-index:2}"
".gmeta{display:flex;justify-content:space-between;align-items:center;"
"padding:6px 8px;font-size:.72rem;color:#9ab}"
".gmeta button{background:none;border:none;color:#e77;cursor:pointer;font-size:.9rem}"
".gmeta .gidbtn{color:#7fc98b}"
".gid{padding:0 8px 7px;font-size:.72rem;color:#cde;line-height:1.35}"
".gid .gidt{display:block;color:#9ab;font-size:.66rem}"
".glabel.gconf{background:rgba(30,70,40,.92);color:#8fe0a0;font-weight:600}"
".glabel.gcls{background:rgba(74,62,22,.9);color:#e8d494}"   /* model-classified, unconfirmed */
".glabel.gunc{background:rgba(44,46,52,.82);color:#9aa6b0}"  /* unclassified */
".glabel.gnb{background:rgba(64,42,42,.86);color:#d6a6a6}"   /* model says no bird (review) */
".glabel.gnbc{background:rgba(40,58,50,.9);color:#a6cbb6;font-weight:600}"  /* confirmed no-bird */
".glabel.goth{background:rgba(58,42,64,.9);color:#c6a6d6;font-weight:600}"  /* other/not-a-bird (hard negative) */
".glabel.gunk{background:rgba(58,52,40,.9);color:#d6c69a;font-weight:600}"  /* unknown/bad-bird (excluded) */
"#grid.nbmode .gitem>a{pointer-events:none;cursor:default}"   /* dbl-click marks no-bird */
"#grid.nbmode .gitem{cursor:pointer}"
".gflt{background:#24402c;color:#cfe;border:1px solid #3a5a42;border-radius:5px;"
"padding:3px 6px;font-size:.8rem;margin:0}"
".grl{display:flex;gap:6px;padding:0 8px 8px}"
".grl .rlin{flex:1;min-width:0;padding:4px 6px;background:#12261a;border:1px solid #2a4d34;"
"color:#dfe;border-radius:4px;font-size:.74rem}"
".grl .rlok{background:#2f6b3f;border:none;color:#eafff0;border-radius:4px;padding:4px 10px;"
"cursor:pointer;font-size:.74rem}"
"h3.sh{color:#7fc98b;font-size:.9rem;margin:18px 0 6px}"
".srow{display:flex;align-items:center;gap:8px;font-size:.78rem;margin:3px 0}"
".srow .lbl{width:84px;color:#9ab;flex-shrink:0}"
".drow{display:flex;justify-content:space-between;gap:12px;font-size:.8rem;"
"padding:4px 0;border-bottom:1px solid #2a4d34}"
".drow span:first-child{color:#9ab}"
".ok{color:#7fc98b}.bad{color:#e77}"
".sbar{background:#3f8a4f;height:14px;border-radius:3px;min-width:2px}"
".hwrap{display:flex;align-items:flex-end;gap:2px;height:110px}"
".hcol{flex:1;display:flex;flex-direction:column;justify-content:flex-end;height:100%}"
".hfill{background:#3f8a4f;border-radius:2px 2px 0 0}"
".hlbl{font-size:.6rem;color:#9ab;text-align:center;margin-top:2px;height:12px}"
"table.st{border-collapse:collapse;font-size:.8rem;width:100%}"
"table.st th,table.st td{padding:5px 8px;text-align:left;border-bottom:1px solid #2a4d34}"
"table.st th{color:#7fc98b;font-size:.72rem;text-transform:uppercase}"
"table.st tr:hover td{background:#1e3826}"
".fpb{font-size:.62rem;background:#5a3030;color:#e0a0a0;padding:1px 5px;border-radius:3px;"
"text-transform:uppercase;vertical-align:middle}"
".wl{display:block;margin:10px 0 4px;font-size:.85rem}"
".wi{width:100%;max-width:280px;padding:8px;background:#1e3826;border:1px solid #444;"
"border-radius:5px;color:#eee}"
".wr{font-size:.88rem;margin-right:18px}"
".wnet{background:#2a4d34;border-radius:5px;padding:8px 12px;margin-bottom:4px;max-width:320px;"
"cursor:pointer;display:flex;justify-content:space-between;align-items:center;font-size:.85rem}"
".wnet:hover{background:#356343}"
"</style></head><body>"
"<div class='hdr'><h1>&#128038; " FIRMWARE_NAME "</h1>"
"<span class='v'>v" FIRMWARE_VERSION "</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"livep\",this)'>Live</button>"
"<button class='tab' onclick='show(\"galleryp\",this)'>Gallery</button>"
"<button class='tab' onclick='show(\"statsp\",this)'>Stats</button>"
"<button class='tab' onclick='show(\"setp\",this)'>Settings</button>"
"<button class='tab' onclick='show(\"dbgp\",this)'>Debug</button>"
"<button class='tab' onclick='show(\"wifip\",this)'>WiFi</button>"
"<button class='tab' onclick='show(\"otap\",this)'>OTA Update</button>"
"</div>"
"<div id='livep' class='pane on'>"
"<div class='rotbar'><label class='sts' style='margin:0'>Rotation</label>"
"<select id='lvRot' onchange='lvRotChange()'>"
"<option value='0'>0&deg;</option><option value='1'>90&deg;</option>"
"<option value='2'>180&deg;</option><option value='3'>270&deg;</option>"
"</select></div>"
"<div class='liveWrap' id='liveWrap'>"
"<div class='detbadge' id='detbadge'><span class='dot'></span>DETECTING</div>"
"<div class='pausebadge' id='pausebadge'>&#9208; DETECTION OFF</div>"
"<img class='live' id='live' src='/stream' alt='live stream'"
" onerror='liveErr()' onload='liveOk()'>"
"<div class='livemsg' id='livemsg'></div>"
"<div class='zone' id='zone'></div>"
"<div class='motgrid' id='motgrid'></div>"
"</div>"
"<div class='sts' id='sts'></div>"
"<button class='act' onclick='snap()'>&#128247; Snapshot to SD</button>"
"<button class='act' id='detBtn' data-on='1' onclick='detToggle()'>&#9208; Disable detection</button>"
"<button class='act' id='zoneBtn' onclick='zoneEdit(true)'>&#9638; Edit detection zone</button>"
"<p class='sts' style='margin-top:2px'>Pauses motion detection for maintenance "
"(no visit events fire). The live view and snapshots keep working. Detection "
"re-enables automatically on reboot.</p>"
"<div class='zbar' id='zbar'>"
"<span class='sts' style='margin:0'>Tap cells to include (green) / exclude (red) "
"from motion detection. Excluded cells are ignored, so a swaying branch or busy "
"background won't trigger captures.</span>"
"<button class='act' style='margin:0' onclick='zoneAll(1)'>All on</button>"
"<button class='act' style='margin:0' onclick='zoneAll(0)'>All off</button>"
"<button class='act' style='margin:0;background:#3f8a4f' onclick='zoneSave()'>&#128190; Save zone</button>"
"<button class='act' style='margin:0;background:#555' onclick='zoneEdit(false)'>Cancel</button>"
"<span class='sts' id='zsts' style='margin:0;color:#7fc98b'></span>"
"</div>"
"</div>"
"<div id='galleryp' class='pane'>"
"<div class='gbar'><select id='day' onchange='loadDay()'></select>"
"<button class='act' style='margin:0' onclick='loadDays()'>&#8635; Refresh</button>"
"<button class='act' style='margin:0' onclick='gSelAll()'>&#9745; Select all</button>"
"<button class='act' style='margin:0' onclick='gDelSel()'>&#10060; Delete selected</button>"
"<button class='act' style='margin:0;background:#8a3f3f' onclick='gDelAll()'>&#128465; Delete all</button>"
"<button class='act' style='margin:0;background:#9e3030' onclick='gWipeDay()'>&#9888; Wipe day (photos+stats)</button>"
"<button class='act' style='margin:0' onclick='gRecheck()'>&#128269; Recheck species (day)</button>"
"<button class='act' style='margin:0' onclick='gRecheckSel()'>&#128269; Recheck selected</button>"
"<span class='sts' style='margin:0 0 0 4px'>Show</span>"
"<select id='gfilter' class='gflt' onchange='setFilter(this.value)'>"
"<option value='all'>All</option>"
"<option value='cls'>Classified</option>"
"<option value='conf'>Confirmed (bird)</option>"
"<option value='nb'>No bird (review)</option>"
"<option value='cnb'>Confirmed no-bird</option>"
"<option value='oth'>Other (not a bird)</option>"
"<option value='unk'>Unknown / bad bird</option>"
"<option value='unc'>Unclassified</option>"
"<option value='near'>Near threshold</option></select>"
"<span class='sts' id='gsts' style='margin:0'></span>"
"<span class='sts' id='gselc' style='margin:0;color:#7fc98b'></span></div>"
"<div class='grid' id='grid'></div>"
"<datalist id='speclist'></datalist>"
"</div>"
"<div id='statsp' class='pane'>"
"<h3 class='sh' style='margin-top:0'>Visits per day</h3><div id='sDaily'></div>"
"<h3 class='sh'>Activity by hour of day</h3><div class='hwrap' id='sHourly'></div>"
"<h3 class='sh'>Species</h3><table class='st' id='sSpecies'></table>"
"<div class='sts' id='sTotal'></div>"
"<div id='sImgs'></div>"
"<button class='act' style='margin-left:0;background:#8a3f3f' onclick='statsReset()'>"
"&#128465; Reset Statistics</button>"
"<span class='sts' id='sResetSts'></span>"
"</div>"
"<div id='setp' class='pane'>"
"<h3 class='sh' style='margin-top:0'>Placement</h3>"
"<label class='wr'><input type='radio' name='pmode' value='nestbox' id='stNest'> Nest box</label>"
"<label class='wr'><input type='radio' name='pmode' value='feeder' id='stFeed'> Feeder</label>"
"<h3 class='sh'>Motion &amp; Capture</h3>"
"<label class='wl'>Motion sensitivity: <b id='stSensV'></b></label>"
"<input type='range' min='0' max='100' id='stSens' style='width:100%;max-width:280px'"
" oninput='stSensShow()'>"
"<label class='wl'>Frames per event (1&ndash;10)</label>"
"<input class='wi' type='number' min='1' max='10' id='stCcnt'>"
"<label class='wl'>Frame interval (ms)</label>"
"<input class='wi' type='number' min='250' max='10000' step='250' id='stCivl'>"
"<label class='wl'>Cool-down between events (s)</label>"
"<input class='wi' type='number' min='1' max='600' id='stCool'>"
"<label class='wl'>Detection quarantine after boot (s)</label>"
"<input class='wi' type='number' min='0' max='3600' id='stQtn'>"
"<p class='sts' style='margin-top:2px'>After a reboot, ignore motion for this many seconds so "
"the camera's warm-up frames and the not-yet-synced clock (which files captures under "
"<i>no-date</i>) don't create false events. 0 disables it; 60 is typical.</p>"
"<label class='wl'><input type='checkbox' id='stZoom'> Zoom species ID to the motion area</label>"
"<p class='sts' style='margin-top:2px'>Crops the classifier input to the changed 8&times;8 "
"grid cells so the bird fills more of the model input (better ID). Saved photos are "
"unaffected. Pick which cells count as the detection zone on the Live tab.</p>"
"<label class='wl'><input type='checkbox' id='stFshut'> Reduce motion blur (fast shutter)</label>"
"<p class='sts' style='margin-top:2px'>When the scene reads dark, fixes the camera's exposure "
"short instead of letting it lengthen &mdash; the main cause of a blurry close/fast-moving bird "
"at dusk or indoors. Only engages in low light (same brightness check as the illuminator), so "
"normal daylight shots are unaffected; low-light shots come out darker/noisier in exchange for "
"less blur.</p>"
"<h3 class='sh'>Species Identification</h3>"
"<label class='wl'>Region / species model</label>"
"<select class='wi' id='stRegion'></select>"
"<div class='sts' id='stModels'></div>"
"<label class='wl'>Confidence threshold (%)</label>"
"<input class='wi' type='number' min='30' max='95' id='stConf'>"
"<label class='wl'>Species set</label>"
"<select class='wi' id='stRfilt'>"
"<option value='0'>Global (full model)</option>"
"<option value='1'>Northern Europe only</option></select>"
"<p class='sts' style='margin-top:2px'>Northern Europe restricts IDs to ~80 regional "
"garden/feeder species, so the global model can't return an out-of-region bird "
"(e.g. an American species) by mistake. Needs the default iNaturalist model; ignored "
"for other models.</p>"
"<label class='wl'><input type='checkbox' id='stTta'> Extra-look identification (test-time augmentation)</label>"
"<p class='sts' style='margin-top:2px'>Classifies each frame together with its mirror image and "
"averages the result, giving the model a second look that lifts confidence on hard poses "
"(head-on, backlit). Roughly doubles identification time per frame; saved photos are unaffected.</p>"
"<label class='wl'>Species name language</label>"
"<select class='wi' id='stLang'>"
"<option value='0'>English</option><option value='1'>Norsk (Norwegian)</option>"
"</select>"
"<p class='sts' style='margin-top:2px'>The scientific (Latin) name is always shown too.</p>"
"<h3 class='sh'>Storage &amp; Stream</h3>"
"<label class='wl'>SD retention cap (%)</label>"
"<input class='wi' type='number' min='50' max='95' id='stCap'>"
"<label class='wl'>Stream / capture JPEG quality</label>"
"<select class='wi' id='stQual'>"
"<option value='8'>Best</option><option value='10'>High</option>"
"<option value='12'>Standard</option><option value='18'>Low</option>"
"<option value='25'>Lowest</option></select>"
"<label class='wl'>Resolution (needs reboot)</label>"
"<select class='wi' id='stRes'>"
"<option value='3'>HD 1280&times;720 &mdash; recommended (best motion)</option>"
"<option value='4'>SXGA 1280&times;1024 &mdash; +vertical detail, weaker motion</option>"
"</select>"
"<label class='wl'>Contrast (the OV2640 has no sharpness control)</label>"
"<select class='wi' id='stContrast'>"
"<option value='-2'>-2 (soft)</option><option value='-1'>-1</option>"
"<option value='0'>0 (default)</option><option value='1'>+1</option>"
"<option value='2'>+2 (punchy)</option></select>"
"<label class='wl'>Brightness (auto-exposure level)</label>"
"<select class='wi' id='stAe'>"
"<option value='-2'>-2 (darker)</option><option value='-1'>-1</option>"
"<option value='0'>0 (default)</option><option value='1'>+1</option>"
"<option value='2'>+2 (brighter)</option></select>"
"<p class='sts' style='margin-top:2px'>HD and SXGA share the same 1280px width, so the bird gets "
"the same horizontal detail either way. <b>HD</b> is recommended: its smaller frames keep motion "
"detection the most responsive. <b>SXGA</b> adds vertical detail (a taller frame) but its larger "
"frames slow the motion loop, so it triggers less reliably. Nothing higher is offered &mdash; UXGA "
"leaves no memory for on-device species ID. Resolution takes effect after a reboot; contrast and "
"brightness apply immediately.</p>"
"<label class='wl'>Image rotation (correct mount vs. subject)</label>"
"<select class='wi' id='stRot'>"
"<option value='0'>0&deg;</option><option value='1'>90&deg;</option>"
"<option value='2'>180&deg;</option><option value='3'>270&deg;</option>"
"</select>"
"<p class='sts' style='margin-top:2px'>0&deg;/180&deg; rotate the sensor itself (applies to "
"stream, saved photos, and species ID). 90&deg;/270&deg; aren't supported by this camera's "
"hardware, so only the live view (here) and species ID are corrected; saved photos keep "
"the camera's native orientation.</p>"
"<h3 class='sh'>System</h3>"
"<label class='wl'>Timezone</label>"
"<select class='wi' id='stTz'>"
"<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Central Europe (Oslo/Berlin)</option>"
"<option value='GMT0BST,M3.5.0/1,M10.5.0'>UK / Ireland</option>"
"<option value='EET-2EEST,M3.5.0/3,M10.5.0/4'>Eastern Europe (Helsinki)</option>"
"<option value='UTC0'>UTC</option>"
"<option value='EST5EDT,M3.2.0,M11.1.0'>US Eastern</option>"
"<option value='CST6CDT,M3.2.0,M11.1.0'>US Central</option>"
"<option value='MST7MDT,M3.2.0,M11.1.0'>US Mountain</option>"
"<option value='PST8PDT,M3.2.0,M11.1.0'>US Pacific</option>"
"</select>"
"<label class='wl'>NTP server</label>"
"<select class='wi' id='stNtpSel' onchange='ntpSelChange()'>"
"<option value='pool.ntp.org'>pool.ntp.org (default)</option>"
"<option value='europe.pool.ntp.org'>Europe pool</option>"
"<option value='north-america.pool.ntp.org'>North America pool</option>"
"<option value='asia.pool.ntp.org'>Asia pool</option>"
"<option value='time.cloudflare.com'>time.cloudflare.com</option>"
"<option value='time.google.com'>time.google.com</option>"
"<option value='time.nist.gov'>time.nist.gov</option>"
"<option value='__custom'>Custom&hellip;</option>"
"</select>"
"<input class='wi' id='stNtpCustom' placeholder='ntp.example.com'"
" style='display:none;margin-top:6px'>"
"<label class='wl'>Illuminator LED</label>"
"<select class='wi' id='stIr'>"
"<option value='0'>Off (default)</option><option value='1'>Auto (on during dark hours)</option></select>"
"<p class='sts'>Onboard white/reddish illuminator LED. Auto mode turns it on when the "
"camera's own frames read dark and off again once it's bright, so it doubles as basic "
"night lighting for the feed &mdash; no separate light sensor. Optional; leave off if "
"you don't want it lighting the scene.</p>"
"<p class='sts'>Settings apply immediately &mdash; no reboot needed.</p>"
"<button class='act' style='margin-left:0' onclick='stSave()'>&#128190; Save Settings</button>"
"<span class='sts' id='stSts'></span>"
"<h3 class='sh'>Backup &amp; Restore</h3>"
"<p class='sts'>Download all settings to a file, or restore them from one after a "
"reset or NVS wipe &mdash; so your tuning (zone, sensitivity, confidence&hellip;) "
"survives a re-provision without re-entering it by hand.</p>"
"<button class='act' style='margin-left:0' onclick='cfgExport()'>&#11015;&#65039; Download settings</button>"
"<button class='act' onclick='cfgPick()'>&#11014;&#65039; Restore from file</button>"
"<input type='file' id='cfgFile' accept='.cfg,.txt' style='display:none' onchange='cfgImport(this)'>"
"<span class='sts' id='cfgSts'></span>"
"</div>"
"<div id='dbgp' class='pane'>"
"<h3 class='sh' style='margin-top:0'>System</h3><div id='dSys'></div>"
"<h3 class='sh'>WiFi Link</h3><div id='dWifi'></div>"
"<h3 class='sh'>SD Card</h3><div id='dSd'></div>"
"<h3 class='sh'>Camera</h3><div id='dCam'></div>"
"<h3 class='sh'>Species ID</h3><div id='dCls'></div>"
"<button class='act' style='margin-left:0' onclick='loadDebug()'>&#8635; Refresh</button>"
"<h3 class='sh'>GPIO Test</h3>"
"<p class='sts'>Drives a raw GPIO pin high/low &mdash; use it to find which pin an "
"unlabeled onboard LED is wired to by trying numbers and watching the board. Camera, "
"SD and flash/PSRAM pins are refused; the pin reverts to input on reboot.</p>"
"<label class='wl'>GPIO number</label>"
"<input class='wi' id='dbgGpioNum' type='number' min='0' max='48' value='2' style='max-width:120px'>"
"<button class='act' onclick='dbgGpio(1)'>&#128161; On</button>"
"<button class='act' onclick='dbgGpio(0)'>Off</button>"
"<span class='sts' id='dbgGpioSts'></span>"
"</div>"
"<div id='wifip' class='pane'>"
"<h3 class='sh' style='margin-top:0'>WiFi Network</h3>"
"<div class='sts' id='wfConn'>&hellip;</div>"
"<button class='act' style='margin:6px 0 6px' onclick=\"wfScan('wfSid','wfNets','wfSts')\">&#128246; Scan Networks</button>"
"<div class='sts' id='wfSts'></div><div id='wfNets'></div>"
"<form action='/wifi-save' method='POST'>"
"<input type='hidden' name='slot' value='0'>"
"<label class='wl'>Primary SSID</label>"
"<input class='wi' id='wfSid' name='ssid' placeholder='tap network or type' required>"
"<label class='wl'>Password</label>"
"<input class='wi' name='pass' type='password' placeholder='leave blank if open'>"
"<br><button class='act' type='submit'>Connect &amp; Save</button></form>"
"<h3 class='sh'>Alternative Network (alt1)</h3>"
"<p class='sts'>A fallback AP, tried when the primary is unreachable and used for "
"failover in service. Shares the IP setting below &mdash; a single static address "
"only works if both APs are on the same subnet, otherwise use DHCP. Save with a "
"blank SSID to remove it.</p>"
"<button class='act' style='margin:0 0 6px' onclick=\"wfScan('wfSid2','wfNets2','wfSts2')\">&#128246; Scan Networks</button>"
"<div class='sts' id='wfSts2'></div><div id='wfNets2'></div>"
"<form action='/wifi-save' method='POST'>"
"<input type='hidden' name='slot' value='1'>"
"<label class='wl'>Alternative SSID</label>"
"<input class='wi' id='wfSid2' name='ssid' placeholder='tap network or type'>"
"<label class='wl'>Password</label>"
"<input class='wi' name='pass' type='password' placeholder='leave blank if open'>"
"<br><button class='act' type='submit'>Save Alternative</button></form>"
"<h3 class='sh'>IP Configuration</h3>"
"<form action='/api/ipconfig/save' method='POST'>"
"<label class='wr'><input type='radio' name='mode' value='dhcp' id='ipDhcp' checked "
"onchange='ipModeChange()'> Automatic (DHCP)</label>"
"<label class='wr'><input type='radio' name='mode' value='static' id='ipStatic' "
"onchange='ipModeChange()'> Static IP</label>"
"<div id='ipFields' style='display:none'>"
"<label class='wl'>IP Address</label><input class='wi' name='ip' id='ipAddr'>"
"<label class='wl'>Subnet Mask</label><input class='wi' name='mask' id='ipMask'>"
"<label class='wl'>Gateway</label><input class='wi' name='gw' id='ipGw'>"
"<label class='wl'>DNS Server</label><input class='wi' name='dns' id='ipDns' placeholder='optional'>"
"</div>"
"<p class='sts'>Saving reboots the device to apply. If a static IP is unreachable afterward, "
"hold the boot button 5 s while powering on to reset WiFi + IP settings.</p>"
"<button class='act' type='submit'>Save &amp; Apply</button></form>"
"<h3 class='sh'>Device</h3>"
"<button class='act' onclick='rebootDev()'>&#128260; Reboot Now</button>"
"</div>"
"<div id='otap' class='pane'>"
"<h3 class='sh' style='margin-top:0'>Firmware Update</h3>"
"<p class='sts'>Running version: v" FIRMWARE_VERSION "</p>"
"<p class='sts'>Upload a birdbox-vX.Y.Z.bin release. The device flashes it to the "
"inactive slot and reboots into it immediately; if that image never boots cleanly, "
"the previous version resumes automatically on the next boot (dual OTA partitions, "
"FSD &sect;8) &mdash; a bad upload can't brick the device. Don't power-cycle mid-upload.</p>"
"<input type='file' id='otaFile' accept='.bin'>"
"<button class='act' onclick='otaUpload()'>&#11014; Upload &amp; Flash</button>"
"<div class='sts' id='otaProg'></div>"
"</div>"
"<script>"
"function show(id,btn){"
"document.querySelectorAll('.pane').forEach(e=>e.classList.remove('on'));"
"document.querySelectorAll('.tab').forEach(e=>e.classList.remove('on'));"
"document.getElementById(id).classList.add('on');btn.classList.add('on');"
"var lv=document.getElementById('live');"
"if(id==='livep'){if(!lv.src.endsWith('/stream'))lv.src='/stream';}"
"else{lv.src='';}"   /* stop streaming while hidden — frees a stream slot */
"if(id==='galleryp'){loadDays();gRcPoll();"
"if(!g_nbBound){var gg=$g('grid');if(gg){gg.addEventListener('dblclick',nbDbl);g_nbBound=true;}}}"
"if(id==='statsp')loadStats();"
"if(id==='setp')stLoad();"
"if(id==='dbgp')loadDebug();"
"if(id==='wifip'){ipLoad();wfCfg();}}"
"function tick(){fetch('/api/status').then(r=>r.json()).then(s=>{"
"var t=(s.time?('\\uD83D\\uDD52 '+s.time+' ('+s.clockSrc+')'):'\\uD83D\\uDD52 clock not set')+' | '"
"+s.ip+' | RSSI '+s.rssi+' dBm | heap '+Math.round(s.heap/1024)+' KB | up '+s.uptime+' s'"
"+' | SD '+(s.sdPresent?s.sdFreeMB+' MB free':'none')"
"+' | motion '+(s.motion?'ACTIVE':'idle')+' | events '+s.events"
"+(s.quarantineS>0?(' | \\u23F3 quarantine '+s.quarantineS+'s'):'');"
"if(s.lastEvent)t+=' | last: <a href=\"'+s.lastEvent+'\">'+s.lastEvent.split('/').pop()+'</a>';"
"if(s.species)t+=' ('+s.species+')';"
"document.getElementById('sts').innerHTML=t;"
"var db=$g('detbadge'),lw=$g('liveWrap');"
"if(db)db.classList.toggle('on',!!s.motion);"
"if(lw)lw.classList.toggle('det',!!s.motion);"
"if(s.quarantineS>0){var pb=$g('pausebadge');"
"if(pb){pb.innerHTML='\\u23F3 BOOT QUARANTINE '+s.quarantineS+'s';pb.classList.add('on');}}"
"else if(typeof s.detect!=='undefined')detApply(!!s.detect);"
"var lm=$g('livemsg');"
"if(lm&&lm.classList.contains('on')&&s.streamUsed<s.streamMax)liveRetry();"   /* slot freed — reconnect */
"}).catch(()=>{});}tick();setInterval(tick,2000);"
/* Fast per-trigger motion border: poll the tiny /api/motion ~2Hz while the live
 * tab is open; a rising trigger count flashes a red frame border for 1s. */
"var g_motN=-1;"
"function motGrid(){var g=$g('motgrid');if(!g)return null;"
"if(g.children.length!==64){g.innerHTML='';"
"for(var c=0;c<64;c++){var d=document.createElement('div');d.className='mcell';g.appendChild(d);}}return g;}"
"function motClear(){var g=$g('motgrid');if(g)for(var c=0;c<g.children.length;c++)g.children[c].className='mcell';}"
"function motTick(){var lp=$g('livep');if(!lp||!lp.classList.contains('on'))return;"
"fetch('/api/motion').then(r=>r.json()).then(function(m){"
"if(g_motN<0){g_motN=m.n;return;}"           /* baseline on first poll — don't flash stale */
"if(m.n>g_motN){g_motN=m.n;motFlash(m.c);}"
"}).catch(function(){});}"
/* Border flash (whole frame) + highlight the exact 8x8 cells that fired, for 1s. */
"function motFlash(cells){var lw=$g('liveWrap');if(!lw)return;lw.classList.add('mot');"
"var g=motGrid();if(g&&cells&&cells.length===64)"
"for(var c=0;c<64;c++)g.children[c].className=(cells[c]==='1'?'mcell hit':'mcell');"
"clearTimeout(window._motT);window._motT=setTimeout(function(){lw.classList.remove('mot');motClear();},1000);}"
"setInterval(motTick,500);"
"function liveOk(){var m=$g('livemsg');if(m)m.classList.remove('on');}"
"function liveErr(){var lv=$g('live');if(!lv||lv.src.indexOf('/stream')<0)return;"   /* src cleared on tab switch — ignore */
"var m=$g('livemsg');if(!m)return;m.classList.add('on');m.innerHTML='\\u23F3 connecting\\u2026';"
"fetch('/api/status').then(r=>r.json()).then(function(s){var rb='<br><button onclick=\"liveRetry()\">Retry</button>';"
"if(s.streamUsed>=s.streamMax)m.innerHTML='\\uD83D\\uDCF5 Live view busy<div class=lms>All '+s.streamMax+' stream slots are in use by other viewers. It will reconnect automatically when one frees.</div>'+rb;"
"else m.innerHTML='\\u26A0\\uFE0F No video<div class=lms>Camera stream unavailable.</div>'+rb;"
"}).catch(function(){m.innerHTML='\\u26A0\\uFE0F No connection to the device<br><button onclick=\"liveRetry()\">Retry</button>';});}"
"function liveRetry(){var lv=$g('live');if(lv)lv.src='/stream?t='+Date.now();}"
"function detApply(en){var b=$g('detBtn');if(b){b.dataset.on=en?'1':'0';"
"b.innerHTML=en?'\\u23F8 Disable detection':'\\u25B6 Enable detection';"
"b.classList.toggle('off',!en);}"
"var pb=$g('pausebadge');if(pb){pb.innerHTML='\\u23F8 DETECTION OFF';pb.classList.toggle('on',!en);}}"
"function detToggle(){var b=$g('detBtn');var on=b.dataset.on==='1';b.disabled=true;"
"fetch('/api/detect',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'enabled='+(on?'0':'1')}).then(r=>r.json()).then(o=>{"
"b.disabled=false;detApply(!!o.enabled);})"
".catch(()=>{b.disabled=false;});}"
"function setTime(cb){fetch('/api/time',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'epoch='+Math.floor(Date.now()/1000)}).then(function(){cb&&cb();},function(){cb&&cb();});}"
"setTime();"
"function snap(){setTime(function(){fetch('/api/capture',{method:'POST'}).then(r=>r.json())"
".then(o=>{alert(o.path?('Saved '+o.path):JSON.stringify(o));}).catch(()=>alert('failed'));});}"
"function applyRot(v){v=String(v);var w=$g('liveWrap'),l=$g('live');"
"w.classList.remove('r1','r3');l.classList.remove('r1','r3');"
"if(v==='1'){w.classList.add('r1');l.classList.add('r1');}"
"if(v==='3'){w.classList.add('r3');l.classList.add('r3');}}"
"function lvRotChange(){var v=$g('lvRot').value;applyRot(v);"
"if($g('stRot'))$g('stRot').value=v;"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'rot='+v});}"
"var g_zone=[];"
"function loadRot(){fetch('/api/settings').then(r=>r.json()).then(function(c){"
"$g('lvRot').value=c.rot;applyRot(c.rot);"
"if(c.zone&&c.zone.length===64)g_zone=c.zone.split('').map(function(x){return x==='1';});"
"if(g_zoneEditing)zoneBuild();}).catch(()=>{});}"
"loadRot();"
"var g_zoneEditing=false;"
"function zoneBuild(){var z=$g('zone');z.innerHTML='';"
"for(var c=0;c<64;c++){var d=document.createElement('div');d.className='zcell';"
"d.dataset.c=c;d.onclick=zoneTap;z.appendChild(d);}zoneRender();}"
"function zoneRender(){var cells=$g('zone').children;"
"for(var c=0;c<64;c++)cells[c].className='zcell '+(g_zone[c]!==false?'inz':'off');}"
"function zoneTap(e){var c=+e.target.dataset.c;g_zone[c]=(g_zone[c]===false);zoneRender();}"
"function zoneAll(v){for(var c=0;c<64;c++)g_zone[c]=!!v;zoneRender();}"
"function zoneEdit(on){g_zoneEditing=on;"
"$g('zone').classList.toggle('on',on);$g('zbar').classList.toggle('on',on);"
"$g('zoneBtn').style.display=on?'none':'';"
"if(on){if(g_zone.length!==64){g_zone=[];for(var c=0;c<64;c++)g_zone[c]=true;}zoneBuild();}"
"else{loadRot();}}"   /* reload authoritative zone, discarding unsaved edits */
"function zoneSave(){var s='';for(var c=0;c<64;c++)s+=(g_zone[c]!==false?'1':'0');"
"$g('zsts').textContent='saving\\u2026';"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'zone='+s})"
".then(r=>r.json()).then(function(o){$g('zsts').textContent=o.ok?'Saved \\u2713':'Save failed';"
"g_zoneEditing=false;setTimeout(function(){$g('zsts').textContent='';"
"$g('zone').classList.remove('on');$g('zbar').classList.remove('on');"
"$g('zoneBtn').style.display='';},900);})"
".catch(function(){$g('zsts').textContent='Save failed';});}"
"function loadDays(){"
"fetch('/api/settings').then(r=>r.json()).then(function(c){g_conf=c.conf;if(g_gfilter==='near')applyFilter();}).catch(()=>{});"
"fetch('/api/days').then(r=>r.json()).then(a=>{"
"a.sort((x,y)=>y.d.localeCompare(x.d));"
"var s=document.getElementById('day');var prev=s.value;s.innerHTML='';"
"a.forEach(o=>{var op=document.createElement('option');op.value=o.d;"
"op.textContent=o.d+' ('+o.n+')';s.appendChild(op);});"
"if(prev&&[...s.options].some(o=>o.value===prev))s.value=prev;"
"if(a.length)loadDay();else{document.getElementById('grid').innerHTML='';"
"document.getElementById('gsts').textContent='no captures yet';}});}"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\"/g,'&quot;');}"
"function loadDay(){var d=$g('day').value;if(!d)return;"
"fetch('/api/events?date='+d).then(r=>r.json()).then(a=>{"
"a.sort((x,y)=>y.f.localeCompare(x.f));"
"var nC=a.filter(o=>o.st===1).length,nF=a.filter(o=>o.st===2).length,nB=a.filter(o=>o.st===3).length,nX=a.filter(o=>o.st===4).length,nO=a.filter(o=>o.st===5).length,nU=a.filter(o=>o.st===6).length;"
"g_gbase=a.length+' capture'+(a.length!==1?'s':'')+' \\u00b7 '+nF+' confirmed \\u00b7 '+nC+' classified \\u00b7 '+nB+' no-bird'+(nX?(' \\u00b7 '+nX+' conf.no-bird'):'')+(nO?(' \\u00b7 '+nO+' other'):'')+(nU?(' \\u00b7 '+nU+' unknown'):'');"
"var g=$g('grid');g.innerHTML='';"
"a.forEach(o=>{var p='/captures/'+d+'/'+o.f;var st=o.st||0;var nm=o.sp||'';var pc=' '+(o.pct||0)+'%';"
"var div=document.createElement('div');div.className='gitem';div.dataset.st=st;div.dataset.sp=nm;div.dataset.f=o.f;div.dataset.pct=(o.pct==null?-1:o.pct);"
"var bcl=st===2?'gconf':st===1?'gcls':st===3?'gnb':st===4?'gnbc':st===5?'goth':st===6?'gunk':'gunc';"
"var btxt=st===2?('\\u2713 '+esc(nm)):st===4?('\\u2713 '+esc(nm||'no bird')):st===5?'\\u2713 not a bird':st===6?'\\u2713 unknown bird':st===1?(esc(nm)+pc):st===3?(esc(nm)+pc):(nm?esc(nm):'unclassified');"
"var bttl=st===2?(esc(nm)+' \\u2013 confirmed'):st===4?(esc(nm||'no bird')+' \\u2013 no bird (confirmed)'):st===5?'other / not a bird \\u2013 hard negative':st===6?'unknown / bad bird \\u2013 excluded from training':st===1?(esc(nm)+pc+' \\u2013 classified'):st===3?(esc(nm)+pc+' \\u2013 no bird (model \\u2013 review)'):'unclassified';"
"var bdg='<div class=\"glabel '+bcl+'\" title=\"'+bttl+'\">'+btxt+'</div>';"
"var cfb=st===1?('<button class=\"gidbtn gcfb\" title=\"confirm this species\" data-d=\"'+d+'\" data-f=\"'+esc(o.f)+'\" onclick=\"confirmSp(this)\">\\u2713</button>')"
":st===3?('<button class=\"gidbtn gcfb\" title=\"confirm: no bird\" data-d=\"'+d+'\" data-f=\"'+esc(o.f)+'\" onclick=\"confirmNoBird(this)\">\\u2713</button>'):'';"
"div.innerHTML=bdg+'<input type=\"checkbox\" class=\"gchk\" data-f=\"'+esc(o.f)+'\" "
"onchange=\"gSelSync(this)\">"
"<a href=\"'+p+'\" target=\"_blank\"><img loading=\"lazy\" src=\"'+p+'\"></a>"
"<div class=\"gmeta\"><span>'+o.f+' &middot; '+Math.round(o.s/1024)+' KB</span>"
"<span><button class=\"gidbtn\" title=\"open full image (new tab)\" "
"onclick=\"openFull(\\''+p+'\\')\">&#128065;</button>"
"<button class=\"gidbtn\" title=\"identify bird\" "
"onclick=\"idBird(this,\\''+d+'\\',\\''+o.f+'\\')\">&#128269;</button>'+cfb+'"
"<button class=\"gidbtn\" title=\"set/correct species\" data-d=\"'+d+'\" "
"data-f=\"'+esc(o.f)+'\" data-sp=\"'+esc(nm)+'\" onclick=\"reLabel(this)\">&#9998;</button>"
"<button class=\"gidbtn gcpl\" data-d=\"'+d+'\" data-f=\"'+esc(o.f)+'\" "
"onclick=\"copyLast(this)\"'+(g_lastSp?' title=\"copy last: '+esc(g_lastSp.d)+'\"':' title=\"copy last species (label one first)\" disabled')+'>&#128203;</button>"
"<button title=\"delete\" onclick=\"del(\\''+p+'\\')\">&#10060;</button></span></div>';"
"g.appendChild(div);});gSelSync();applyFilter();});}"
"var g_gfilter='all',g_gbase='',g_nbBound=false,g_conf=17,g_lastSp=null;"
"function setFilter(v){g_gfilter=v;$g('grid').classList.toggle('nbmode',v==='unc'||v==='near');applyFilter();}"
"function applyFilter(){var items=[...document.querySelectorAll('#grid .gitem')],vis=0,lo=g_conf-5;"
"items.forEach(function(it){var st=+(it.dataset.st||0),p=+(it.dataset.pct);"
"var near=st===0&&p>=lo&&p<g_conf;"   /* top-1 within 5% below the ID threshold */
"var show=g_gfilter==='all'||(g_gfilter==='cls'&&st===1)||(g_gfilter==='conf'&&st===2)||(g_gfilter==='nb'&&st===3)||(g_gfilter==='cnb'&&st===4)||(g_gfilter==='oth'&&st===5)||(g_gfilter==='unk'&&st===6)||(g_gfilter==='unc'&&st===0)||(g_gfilter==='near'&&near);"
"it.style.display=show?'':'none';"
"if(show){vis++;}else{var cb=it.querySelector('.gchk');if(cb&&cb.checked){cb.checked=false;it.classList.remove('sel');}}});"
"$g('gsts').textContent=g_gbase+(g_gfilter!=='all'?(' \\u00b7 '+vis+' shown'):'')"
"+(g_gfilter==='unc'?' \\u00b7 double-click a tile = no bird':'')"
"+(g_gfilter==='near'?(' \\u00b7 top guess '+lo+'\\u2013'+(g_conf-1)+'% (just under the '+g_conf+'% threshold) \\u00b7 double-click = no bird'):'');gSelSync();}"
"function confirmSp(btn){var d=btn.dataset.d,f=btn.dataset.f,it=btn.closest('.gitem');btn.disabled=true;"
"fetch('/api/confirm',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f)})"
".then(r=>r.json()).then(o=>{if(o.ok){var b=it.querySelector('.glabel');var nm=it.dataset.sp||'';"
"if(b){b.className='glabel gconf';b.textContent='\\u2713 '+nm;b.title=nm+' \\u2013 confirmed';}"
"it.dataset.st=2;btn.remove();applyFilter();}else{btn.disabled=false;alert(o.error||'Nothing to confirm');}})"
".catch(()=>{btn.disabled=false;});}"
"function markNoBird(d,f,it,btn){if(btn)btn.disabled=true;"
"fetch('/api/relabel',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f)+'&c='+encodeURIComponent('no bird')+'&l='})"
".then(r=>r.json()).then(o=>{if(o.ok){var b=it.querySelector('.glabel');var nm=it.dataset.sp||'no bird';"
"if(!b){b=document.createElement('div');b.className='glabel';it.insertBefore(b,it.firstChild);}"
"b.className='glabel gnbc';b.textContent='\\u2713 '+nm;b.title=nm+' \\u2013 no bird (confirmed)';"
"it.dataset.st=4;var cb=it.querySelector('.gcfb');if(cb)cb.remove();applyFilter();}"
"else if(btn){btn.disabled=false;alert(o.error||'Failed');}}).catch(()=>{if(btn)btn.disabled=false;});}"
"function confirmNoBird(btn){markNoBird(btn.dataset.d,btn.dataset.f,btn.closest('.gitem'),btn);}"
"function nbDbl(e){var g=$g('grid');if(!g.classList.contains('nbmode'))return;"
"if(e.target.closest('button,input,.grl'))return;"
"var it=e.target.closest('.gitem');if(!it||it.dataset.st!=='0')return;"
"e.preventDefault();markNoBird($g('day').value,it.dataset.f,it,null);}"
"function idBird(btn,d,f){var it=btn.closest('.gitem');"
"var out=it.querySelector('.gid');"
"if(!out){out=document.createElement('div');out.className='gid';it.appendChild(out);}"
"out.textContent='\\u2026 identifying';btn.disabled=true;"
"fetch('/api/classify-file?date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f))"
".then(r=>r.json()).then(o=>{btn.disabled=false;"
"if(o.error){out.textContent=o.error;return;}"
"var t=(o.top3||[]).filter(x=>x.pct>0).map(x=>esc(x.label)+' '+x.pct+'%').join(', ');"
"out.innerHTML='<b>'+esc(o.species||'?')+'</b> '+(o.confidence||0)+'%'"
"+(o.saved?' \\u2013 saved \\u2713':'')+(t?'<span class=gidt>'+t+'</span>':'');"
"if(o.saved){var bdg=it.querySelector('.glabel');"
"if(!bdg){bdg=document.createElement('div');bdg.className='glabel';it.insertBefore(bdg,it.firstChild);}"
"bdg.className='glabel gconf';bdg.textContent='\\u2713 '+(o.species||'');"
"bdg.title=(o.species||'')+' \\u2013 confirmed';it.dataset.st=2;it.dataset.sp=(o.species||'');"
"var cb=it.querySelector('.gcfb');if(cb)cb.remove();applyFilter();}})"
".catch(()=>{btn.disabled=false;out.textContent='identify failed';});}"
"var g_specMap=null;"   /* display name -> {c:common,l:latin}, loaded once */
"function ensureSpecs(cb){if(g_specMap){cb();return;}"
"fetch('/api/labels').then(r=>r.json()).then(function(a){g_specMap={};"
"var dl=$g('speclist');dl.innerHTML='<option value=\"No bird\"><option value=\"Other (not a bird)\"><option value=\"Unknown bird\">'+a.map(o=>'<option value=\"'+esc(o.d)+'\">').join('');"
"g_specMap['No bird']={c:'no bird',l:''};"
"g_specMap['Other (not a bird)']={c:'other',l:''};"
"g_specMap['Unknown bird']={c:'unknown',l:''};"
"a.forEach(o=>{g_specMap[o.d]={c:o.c,l:o.l};});cb();}).catch(()=>{g_specMap={};cb();});}"
"function reLabel(btn){var d=btn.dataset.d,f=btn.dataset.f,cur=btn.dataset.sp;"
"var it=btn.closest('.gitem');var box=it.querySelector('.grl');"
"if(box){box.remove();return;}"   /* toggle closed */
"ensureSpecs(function(){var b=document.createElement('div');b.className='grl';"
"b.innerHTML='<input list=speclist class=rlin placeholder=\"species\\u2026\">"
"<button class=rlok>Save</button>';it.appendChild(b);"
"var inp=b.querySelector('.rlin');inp.value=cur||'';inp.focus();inp.select();"
"b.querySelector('.rlok').onclick=function(){var v=inp.value.trim();if(!v)return;"
"var m=g_specMap[v]||{c:v,l:''};"
"fetch('/api/relabel',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f)"
"+'&c='+encodeURIComponent(m.c)+'&l='+encodeURIComponent(m.l)})"
".then(r=>r.json()).then(o=>{if(o.ok){g_lastSp={c:m.c,l:m.l,d:v};loadDay();}else alert(o.error||'Relabel failed');})"
".catch(()=>alert('Relabel failed'));};"
"inp.addEventListener('keydown',function(e){if(e.key==='Enter')b.querySelector('.rlok').click();});});}"
/* Apply the last MANUALLY entered species to another tile in one click — the
 * fast path when a run of unclassified frames is all the same bird. Enabled
 * only once a manual relabel has set g_lastSp; updates the tile in place so it
 * leaves the Unclassified/Near filter as you go (§3.4). */
"function copyLast(btn){if(!g_lastSp)return;var d=btn.dataset.d,f=btn.dataset.f,"
"it=btn.closest('.gitem'),m=g_lastSp;btn.disabled=true;"
"fetch('/api/relabel',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f)"
"+'&c='+encodeURIComponent(m.c)+'&l='+encodeURIComponent(m.l)})"
".then(r=>r.json()).then(o=>{if(o.ok){var b=it.querySelector('.glabel');"
"if(!b){b=document.createElement('div');b.className='glabel';it.insertBefore(b,it.firstChild);}"
"b.className='glabel gconf';b.textContent='\\u2713 '+m.d;b.title=m.d+' \\u2013 confirmed';"
"it.dataset.st=2;it.dataset.sp=m.d;var cb=it.querySelector('.gcfb');if(cb)cb.remove();applyFilter();}"
"else{btn.disabled=false;alert(o.error||'Copy failed');}}).catch(()=>{btn.disabled=false;});}"
"function gChecks(){return [...document.querySelectorAll('#grid .gchk')];}"
"function gVisChecks(){return gChecks().filter(c=>c.closest('.gitem').style.display!=='none');}"
"function gSelSync(cb){if(cb)cb.closest('.gitem').classList.toggle('sel',cb.checked);"
"var n=gChecks().filter(c=>c.checked).length;"
"$g('gselc').textContent=n?(n+' selected'):'';}"
"function gSelAll(){var c=gVisChecks();var on=c.some(x=>!x.checked);"
"c.forEach(x=>{x.checked=on;x.closest('.gitem').classList.toggle('sel',on);});gSelSync();}"
"function gDelSel(){var d=$g('day').value;"
"var fs=gChecks().filter(c=>c.checked).map(c=>c.dataset.f);"
"if(!fs.length){alert('No images selected');return;}"
"if(!confirm('Delete '+fs.length+' selected image'+(fs.length!==1?'s':'')+'? Cannot be undone.'))return;"
"fetch('/api/captures/delete',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&files='+fs.join(',')})"
".then(r=>r.json()).then(()=>loadDays()).catch(()=>alert('Delete failed'));}"
"function gDelAll(){var d=$g('day').value;if(!d)return;"
"if(!confirm('Delete ALL photos in '+d+'? Every image for that day is removed "
"and this cannot be undone. Statistics are separate \\u2014 use the Stats tab\\u2019s "
"Reset Statistics to clear those.'))return;"
"fetch('/api/captures/delete',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&all=1'})"
".then(r=>r.json()).then(()=>loadDays()).catch(()=>alert('Delete failed'));}"
"function gWipeDay(){var d=$g('day').value;if(!d)return;"
"if(!confirm('Wipe '+d+' completely? This deletes every photo AND removes that "
"day\\u2019s entries from the statistics/visit log. Cannot be undone.'))return;"
"fetch('/api/captures/delete',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&all=1&stats=1'})"
".then(r=>r.json()).then(o=>{alert('Removed '+(o.deleted||0)+' photo(s) and '"
"+(o.statsRemoved||0)+' log row(s).');loadDays();}).catch(()=>alert('Wipe failed'));}"
"function del(p){if(!confirm('Delete '+p.split('/').pop()+'?'))return;"
"fetch(p,{method:'DELETE'}).then(()=>loadDays());}"
/* Open the full capture in a new tab — a per-tile button so it works in the
 * Unclassified/Near filter too, where the image's own link is disabled to keep
 * the double-click = no-bird gesture clean (§3.4). window.open in a click
 * handler is a direct user gesture, so it isn't popup-blocked. */
"function openFull(p){window.open(p,'_blank');}"
"function gRcStart(d,files){fetch('/api/recheck',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+(files?'&files='+files:'')}).then(r=>r.json())"
".then(o=>{if(!o.ok){alert(o.error||'Recheck failed');return;}gRcPoll();})"
".catch(()=>alert('Recheck failed'));}"
"function gRcDay(d){if(!d)return false;"
"if(d==='no-date'){alert('Recheck needs a dated day (rows are matched by timestamp).');return false;}"
"return true;}"
"function gRecheck(){var d=$g('day').value;if(!gRcDay(d))return;"
"if(!confirm('Re-run species ID on all of '+d+'\\u2019s visit-log rows with the "
"current model and settings? Rows are updated in place; user-corrected rows are "
"kept. Takes ~5\\u201310 s per visit.'))return;"
"gRcStart(d,'');}"
"function gRecheckSel(){var d=$g('day').value;if(!gRcDay(d))return;"
"var fs=gChecks().filter(c=>c.checked).map(c=>c.dataset.f);"
"if(!fs.length){alert('No images selected');return;}"
"if(!confirm('Re-run species ID for '+fs.length+' selected photo(s)? A photo with "
"a visit-log row is updated in place; one without (after a stats reset, or a "
"follow-up frame) is ADDED as a new visit row.'))return;"
"gRcStart(d,fs.join(','));}"
"function gRcPoll(){fetch('/api/recheck').then(r=>r.json()).then(function(o){"
"var el=$g('gsts');"
"if(o.busy){el.textContent='Recheck '+o.date+': '+o.done+'/'+o.total+'\\u2026';"
"setTimeout(gRcPoll,2000);}"
"else if(o.date){el.textContent='Recheck '+o.date+' done ('+o.done+'/'+o.total+' row(s))';}"
"});}"
"function loadStats(){"
"Promise.all(['daily','species','hourly'].map(u=>fetch('/api/stats/'+u).then(r=>r.json())))"
".then(function(res){var d=res[0],spo=res[1],sp=spo.rows,h=res[2];"
"d.sort((a,b)=>a.d.localeCompare(b.d));d=d.slice(-30);"
"var m=Math.max(1,...d.map(o=>o.n));"
"document.getElementById('sDaily').innerHTML=d.length?d.map(o=>"
"'<div class=srow><span class=lbl>'+o.d+'</span>"
"<div class=sbar style=\"width:'+Math.round(o.n*80/m)+'%\"></div><span>'+o.n+'</span></div>')"
".join(''):'<span class=sts>no visits logged yet</span>';"
"var hm=Math.max(1,...h);"
"document.getElementById('sHourly').innerHTML=h.map((n,i)=>"
"'<div class=hcol title=\"'+i+':00 &mdash; '+n+'\">"
"<div class=hfill style=\"height:'+Math.round(n*100/hm)+'%\"></div>"
"<div class=hlbl>'+(i%3===0?i:'')+'</div></div>').join('');"
"sp.sort((a,b)=>b.n-a.n);"
"document.getElementById('sSpecies').innerHTML="
"'<tr><th>Species</th><th>Visits</th><th>First seen</th><th>Last seen</th></tr>'+"
"sp.map(o=>'<tr style=cursor:pointer onclick=\"spImgs(\\''+encodeURIComponent(o.key)+"
"'\\',\\''+encodeURIComponent(o.s)+'\\')\"><td>'+esc(o.s)+(o.fp?' <span class=fpb>false pos</span>':'')+"
"'</td><td>'+o.n+'</td><td>'+o.first+'</td><td>'+o.last+'</td></tr>').join('');"
"var birds=sp.filter(o=>!o.fp).reduce((a,o)=>a+o.n,0);"
"document.getElementById('sTotal').textContent=birds+' bird visit(s) total'"
"+(spo.falsePos?' \\u00b7 '+spo.falsePos+' false positive(s)':'')+' \\u2014 click a row for its last 10 images';"
"$g('sImgs').innerHTML='';"
"});}"
"function spImgs(key,name){var nm=decodeURIComponent(name);var el=$g('sImgs');"
"el.innerHTML='<span class=sts>\\u2026 loading images for '+esc(nm)+'</span>';"
"fetch('/api/stats/images?limit=10&sp='+key).then(r=>r.json()).then(function(a){"
"if(!a.length){el.innerHTML='<span class=sts>No images for '+esc(nm)+'</span>';return;}"
"el.innerHTML='<h3 class=sh>Last '+a.length+' image(s): '+esc(nm)+'</h3>'+"
"'<div class=grid>'+a.map(o=>'<div class=gitem>"
"<a href=\"'+o.f+'\" target=_blank><img loading=lazy src=\"'+o.f+'\"></a>"
"<div class=gmeta><span>'+esc((o.t||'').replace('T',' '))+'</span></div></div>').join('')+'</div>';"
"}).catch(()=>{el.innerHTML='<span class=sts>Failed to load images</span>';});}"
"function statsReset(){"
"if(!confirm('Reset all statistics? This permanently deletes the visit-log "
"history (daily/species/hourly charts). Saved photos on SD are not affected. "
"This cannot be undone.'))return;"
"fetch('/api/stats/reset',{method:'POST'}).then(r=>r.json()).then(function(o){"
"$g('sResetSts').textContent=o.ok?'Statistics reset \\u2713':'Reset failed';"
"loadStats();"
"setTimeout(function(){$g('sResetSts').textContent='';},4000);})"
".catch(function(){$g('sResetSts').textContent='Reset failed';});}"
"var g_liveIp='',g_liveMask='',g_liveGw='';"
"function ipLoad(){fetch('/api/ipconfig').then(r=>r.json()).then(c=>{"
"g_liveIp=c.liveIp;g_liveMask=c.liveMask;g_liveGw=c.liveGw;"
"(c.static?document.getElementById('ipStatic'):document.getElementById('ipDhcp')).checked=true;"
"if(c.ip)document.getElementById('ipAddr').value=c.ip;"
"if(c.mask)document.getElementById('ipMask').value=c.mask;"
"if(c.gw)document.getElementById('ipGw').value=c.gw;"
"if(c.dns)document.getElementById('ipDns').value=c.dns;"
"ipModeChange();});}"
"function ipModeChange(){var st=document.getElementById('ipStatic').checked;"
"document.getElementById('ipFields').style.display=st?'block':'none';"
"if(st){"   /* pre-fill empty fields with live values — never clobber typed/saved ones */
"var a=document.getElementById('ipAddr'),m=document.getElementById('ipMask'),"
"g=document.getElementById('ipGw');"
"if(!a.value)a.value=g_liveIp;if(!m.value)m.value=g_liveMask;if(!g.value)g.value=g_liveGw;}}"
"function wfScan(sidId,netsId,stsId){var st=$g(stsId);"
"st.textContent='Scanning\\u2026';$g(netsId).innerHTML='';"
"fetch('/api/scan').then(r=>r.json()).then(function(a){"
"st.textContent=a.length+' network'+(a.length!==1?'s':'')+' found';"
"var c=$g(netsId);"
"a.forEach(function(n){var d=document.createElement('div');d.className='wnet';"
"d.innerHTML='<span>'+n.ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</span>"
"<span class=sts style=\"margin:0\">'+n.rssi+'dBm'+(n.auth?' &#128274;':'')+'</span>';"
"d.onclick=function(){$g(sidId).value=n.ssid;};"
"c.appendChild(d);});}).catch(function(){st.textContent='Scan failed';});}"
"function wfCfg(){fetch('/api/wificfg').then(r=>r.json()).then(function(c){"
"$g('wfConn').textContent='Connected to: '+(c.connected||'\\u2014')"
"+'  \\u2022  Primary: '+(c.primary||'not set')"
"+'  \\u2022  Alt1: '+(c.alt||'not set');}).catch(function(){});}"
"function rebootDev(){if(confirm('Reboot BirdBox?'))"
"fetch('/api/reboot',{method:'POST'}).then(()=>alert('Rebooting\\u2026'));}"
"function $g(i){return document.getElementById(i)}"
"var g_savedRes=1;"
"function stSensShow(){$g('stSensV').textContent=$g('stSens').value;}"
"function stLoad(){fetch('/api/settings').then(r=>r.json()).then(function(c){"
"(c.mode===1?$g('stFeed'):$g('stNest')).checked=true;"
"$g('stSens').value=c.sens;stSensShow();"
"$g('stCcnt').value=c.ccnt;$g('stCivl').value=c.civl;$g('stCool').value=c.cool;"
"$g('stQtn').value=c.qtn;"
"$g('stConf').value=c.conf;$g('stCap').value=c.cap;$g('stIr').value=c.ir;"
"$g('stLang').value=c.lang;$g('stZoom').checked=c.dzoom==1;"
"$g('stFshut').checked=c.fshut==1;"
"$g('stTta').checked=c.tta==1;"
"$g('stRot').value=c.rot;$g('lvRot').value=c.rot;applyRot(c.rot);"
"$g('stRfilt').value=c.rfilt;"
"var rs=$g('stRes');"
"if(![...rs.options].some(o=>o.value==c.res)){var ro=document.createElement('option');"
"ro.value=c.res;ro.textContent='Current ('+c.res+')';rs.appendChild(ro);}"
"$g('stRes').value=c.res;$g('stContrast').value=c.contrast;$g('stAe').value=c.ae_level;g_savedRes=c.res;"
"var q=$g('stQual');"
"if(![...q.options].some(o=>o.value==c.qual)){var op=document.createElement('option');"
"op.value=c.qual;op.textContent='Custom ('+c.qual+')';q.appendChild(op);}"
"q.value=c.qual;"
"var t=$g('stTz');"
"if(![...t.options].some(o=>o.value===c.tz)){var ot=document.createElement('option');"
"ot.value=c.tz;ot.textContent='Custom: '+c.tz;t.appendChild(ot);}"
"t.value=c.tz;"
"var ns=$g('stNtpSel');"
"if([...ns.options].some(o=>o.value===c.ntp&&o.value!=='__custom')){"
"ns.value=c.ntp;$g('stNtpCustom').style.display='none';}else{"
"ns.value='__custom';$g('stNtpCustom').style.display='block';"
"$g('stNtpCustom').value=c.ntp;}"
"var s=$g('stRegion');s.innerHTML='';"
"var oa=document.createElement('option');oa.value='';"
"oa.textContent='(auto \\u2014 first model found)';s.appendChild(oa);"
"c.models.forEach(function(m){var o=document.createElement('option');"
"o.value=m;o.textContent=m;s.appendChild(o);});"
"if(c.region&&!c.models.includes(c.region)){var om=document.createElement('option');"
"om.value=c.region;om.textContent=c.region+' (missing from SD)';s.appendChild(om);}"
"s.value=c.region;"
"$g('stModels').textContent=c.models.length?"
"c.models.length+' model file'+(c.models.length!==1?'s':'')+' in /model on SD':"
"'no model files in /model on SD \\u2014 species ID arrives in a later firmware';"
"});}"
"function ntpSelChange(){var c=$g('stNtpSel').value==='__custom';"
"$g('stNtpCustom').style.display=c?'block':'none';}"
"function stSave(){"
"var ntp=$g('stNtpSel').value==='__custom'?$g('stNtpCustom').value:$g('stNtpSel').value;"
"var b='mode='+($g('stFeed').checked?'feeder':'nestbox')"
"+'&sens='+$g('stSens').value+'&ccnt='+$g('stCcnt').value"
"+'&civl='+$g('stCivl').value+'&cool='+$g('stCool').value"
"+'&qtn='+$g('stQtn').value"
"+'&conf='+$g('stConf').value+'&cap='+$g('stCap').value"
"+'&rfilt='+$g('stRfilt').value"
"+'&qual='+$g('stQual').value+'&ir='+$g('stIr').value"
"+'&rot='+$g('stRot').value+'&res='+$g('stRes').value"
"+'&contrast='+$g('stContrast').value+'&ael='+$g('stAe').value"
"+'&tz='+encodeURIComponent($g('stTz').value)"
"+'&region='+encodeURIComponent($g('stRegion').value)"
"+'&ntp='+encodeURIComponent(ntp)"
"+'&lang='+$g('stLang').value"
"+'&dzoom='+($g('stZoom').checked?1:0)"
"+'&fshut='+($g('stFshut').checked?1:0)"
"+'&tta='+($g('stTta').checked?1:0);"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
".then(r=>r.json()).then(function(o){"
"$g('stSts').textContent=o.ok?'Saved & applied \\u2713':'Save failed';"
"$g('lvRot').value=$g('stRot').value;applyRot($g('stRot').value);"
"if(o.ok&&$g('stRes').value!==String(g_savedRes)){g_savedRes=+$g('stRes').value;"
"if(confirm('Resolution change needs a reboot to take effect. Reboot now?'))"
"fetch('/api/reboot',{method:'POST'}).then(()=>alert('Rebooting\\u2026'));}"
"setTimeout(function(){$g('stSts').textContent='';},4000);})"
".catch(function(){$g('stSts').textContent='Save failed';});}"
"function cfgExport(){window.location='/api/settings/export';}"
"function cfgPick(){$g('cfgFile').click();}"
"function cfgImport(inp){var f=inp.files[0];if(!f){return;}var rd=new FileReader();"
"rd.onload=function(){fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:rd.result})"
".then(r=>r.json()).then(function(o){"
"$g('cfgSts').textContent=o.ok?'Restored \\u2713':'Restore failed';"
"if(o.ok){stLoad();}"
"setTimeout(function(){$g('cfgSts').textContent='';},5000);})"
".catch(function(){$g('cfgSts').textContent='Restore failed';});};"
"rd.readAsText(f);inp.value='';}"
"function drow(k,v,cls){return '<div class=drow><span>'+k+'</span>"
"<span'+(cls?' class='+cls:'')+'>'+v+'</span></div>';}"
"function fmtAge(s){if(s<0)return 'never';"
"var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);"
"return d?d+'d '+h+'h':h?h+'h '+m+'m':m?m+'m '+(s%60)+'s':s+'s';}"
"function loadDebug(){fetch('/api/sysinfo').then(r=>r.json()).then(function(d){"
"$g('dSys').innerHTML="
"drow('Free heap',Math.round(d.heap/1024)+' KB')+"
"drow('Min free heap (low-water)',Math.round(d.heapMin/1024)+' KB, '+fmtAge(d.heapMinAgo)+' ago')+"
"drow('Uptime',fmtAge(d.uptime))+"
"drow('Last reset',d.resetReason,(d.resetReason==='power-on'||d.resetReason==='software')?'':'bad')+"
"drow('Device time',(d.time?d.time+' ('+d.clockSrc+')':'not set'),(d.time?(d.clockSrc==='ntp'?'ok':''):'bad'))+"
"drow('SoC temperature',(d.socTempC>-100?d.socTempC.toFixed(1)+'\\u00b0C':'n/a'),(d.socTempC>75?'bad':(d.socTempC>-100?'ok':'')))+"
"drow('WiFi reconnects',d.wifiDisc)+"
"drow('Last reconnect',fmtAge(d.wifiDiscAgo)+(d.wifiDiscAgo<0?'':' ago'));"
"$g('dWifi').innerHTML="
"drow('Network',d.apSsid||'\\u2014')+"
"drow('RSSI',d.rssi+' dBm')+drow('Channel',d.ch)+drow('Own MAC',d.mac);"
"$g('dSd').innerHTML=d.sdPresent?"
"drow('Card',d.sdCard)+drow('Size',d.sdTotalMB+' MB total, '+d.sdFreeMB+' MB free')+"
"drow('Last write',d.sdWriteOk?'OK':'FAILED',d.sdWriteOk?'ok':'bad'):"
"drow('Status','no SD card','bad');"
"$g('dCam').innerHTML=d.camPresent?"
"drow('Sensor PID','0x'+d.camPid.toString(16))+drow('Resolution',d.camRes)+"
"drow('JPEG quality',d.camQuality)+"
"drow('Motion triggers',(d.motionTriggers||0))+"
"drow('Watchdog recoveries',(d.camRecoveries||0)+(d.camRecoveryAgo>=0?' (last '+fmtAge(d.camRecoveryAgo)+' ago)':''),(d.camRecoveries?'':'ok'))+"
"(d.camFault?drow('Camera fault','YES \\u2014 needs a manual power cycle','bad'):'')"
":drow('Status','no camera','bad');"
"$g('dCls').innerHTML=d.clsModel?"
"drow('Model',d.clsModel)+drow('Labels',d.clsLabels+' species')+"
"drow('Northern-Europe species',(d.clsRegion||0)+' of '+d.clsLabels"
"+((d.clsRegion||0)?'':' \\u2014 region filter N/A for this model'))+"
"drow('Last inference',d.lastInferenceMs<0?'none yet':d.lastInferenceMs+' ms'):"
"drow('Status','no model loaded \\u2014 upload a .tflite + .txt to /sd/model','bad');"
"}).catch(()=>{});}"
"function dbgGpio(lvl){var n=$g('dbgGpioNum').value;"
"fetch('/api/debug/gpio',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'num='+n+'&level='+lvl}).then(function(r){"
"return r.text().then(function(t){return {ok:r.ok,t:t};});"
"}).then(function(o){"
"$g('dbgGpioSts').textContent=o.ok?('GPIO'+n+' = '+lvl):o.t;"
"}).catch(function(){$g('dbgGpioSts').textContent='request failed';});}"
"function otaUpload(){"
"var f=$g('otaFile').files[0];var p=$g('otaProg');"
"if(!f){p.textContent='Select a .bin file first';return;}"
"var xhr=new XMLHttpRequest();"
"xhr.open('POST','/ota/upload',true);"
"xhr.setRequestHeader('Content-Type','application/octet-stream');"
"xhr.upload.onprogress=function(e){"
"if(e.lengthComputable)p.textContent='Uploading: '+Math.round(e.loaded/e.total*100)+'%';};"
"xhr.onload=function(){p.textContent=xhr.status===200?"
"'Success \\u2014 rebooting\\u2026':'Error: '+xhr.responseText;};"
"xhr.onerror=function(){p.textContent='Upload error';};"
"xhr.send(f);}"
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

/* Has the stream client closed its TCP connection? A browser that navigates
 * away, clears the <img> src, or reloads sends a FIN, but the MJPEG client
 * never sends request data — so any readability on the socket means EOF (recv
 * 0) or error, i.e. the client is gone. Polling this each frame frees the slot
 * promptly; without it the task only noticed on a *send* failure, which a
 * half-open socket can defer indefinitely, leaking the slot until reboot (the
 * "stream busy / no video after a while" bug). */
static bool stream_peer_closed(int fd)
{
    if (fd < 0) return false;
    char b;
    int r = recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
    if (r == 0) return true;                       /* orderly close (FIN) */
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return true;  /* RST/error */
    return false;                                   /* r>0 (unexpected data) or EAGAIN: still open */
}

static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *) arg;
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    int fd = httpd_req_to_sockfd(req);

    /* Bound the ungraceful-disconnect case: if a client vanishes *without*
     * closing (Wi-Fi drop, sleep, power off) it sends no FIN, so the peer-close
     * poll below never sees it; instead its unacked frames fill the TCP send
     * buffer and the next send blocks. SO_SNDTIMEO makes that blocked send fail
     * after 5 s instead of hanging (and holding the slot) until the stack's much
     * longer retransmit timeout. A live client ACKs in milliseconds, so this
     * only ever trips a genuinely dead one. */
    if (fd >= 0) {
        struct timeval snd_to = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    }

    char hdr[64];
    for (;;) {
        if (stream_peer_closed(fd)) break;         /* client gone — free the slot now */
        camera_fb_t *fb = camera_grab();
        if (!fb) {
            ESP_LOGW(TAG, "stream: no frame from camera");
            break;
        }
        int hlen = snprintf(hdr, sizeof(hdr), STREAM_PART_HDR, (unsigned) fb->len);
        bool fail =
            httpd_resp_send_chunk(req, STREAM_BOUNDARY, sizeof(STREAM_BOUNDARY) - 1) != ESP_OK ||
            httpd_resp_send_chunk(req, hdr, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *) fb->buf, fb->len) != ESP_OK;
        camera_return(fb);
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

    /* slot=1 saves the alternative network (alt1); default 0 = primary. The
     * portal form omits it, so absent ⇒ primary. */
    char *slp = strstr(body, "slot=");
    g_new_slot = (slp && slp[5] == '1') ? WIFI_SLOT_ALT : WIFI_SLOT_PRIMARY;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#16281c;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#7fc98b'>Saved!</h2>"
        "<p>Applying and rebooting…</p></div></body></html>");

    g_wifi_save_requested = true;
    return ESP_OK;
}

/* GET /api/wificfg — configured network names (primary + alt1) and the AP the
 * device is currently associated with, for the WiFi tab (FSD §4.7). Passwords
 * are never returned. */
static void json_escape(char *dst, size_t dsz, const char *src)
{
    size_t j = 0;
    for (size_t k = 0; src[k] && j + 2 < dsz; k++) {
        unsigned char c = src[k];
        if (c == '"' || c == '\\') dst[j++] = '\\';
        if (c >= 0x20) dst[j++] = c;
    }
    dst[j] = '\0';
}

static esp_err_t h_wificfg(httpd_req_t *req)
{
    char conn[33] = {0};
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) strlcpy(conn, (char *) ap.ssid, sizeof(conn));

    char p[70], a[70], c[70];
    json_escape(p, sizeof(p), wifi_configured_ssid(WIFI_SLOT_PRIMARY));
    json_escape(a, sizeof(a), wifi_configured_ssid(WIFI_SLOT_ALT));
    json_escape(c, sizeof(c), conn);

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"primary\":\"%s\",\"alt\":\"%s\",\"connected\":\"%s\"}", p, a, c);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* GET /api/portal-status — first-boot portal credential-verify result, so the
 * setup page can show the box's DHCP IP before it reboots onto the LAN
 * (FSD §4.4). state: idle|pending|connected|failed; ip valid when connected. */
static esp_err_t h_portal_status(httpd_req_t *req)
{
    char ip[16] = {0};
    wifi_portal_state_t st = wifi_portal_status(ip, sizeof(ip));
    const char *s = st == WIFI_PORTAL_CONNECTED ? "connected"
                  : st == WIFI_PORTAL_FAILED    ? "failed"
                  : st == WIFI_PORTAL_PENDING   ? "pending" : "idle";
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"ip\":\"%s\"}", s, ip);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
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

/* POST /api/time — set the clock from the connecting browser when SNTP hasn't
 * synced (offline / NTP-blocked network, FSD §3.4). Body: epoch=<UTC seconds>.
 * Only sets the clock while it's unsynced (year < 2020), so a browser with a
 * wrong clock can't override an authoritative SNTP time. Does NOT re-base
 * captures already stranded in /captures/no-date/ — that migration was removed
 * in 2cdc828 after it caused FATFS rename data loss; they stay there. The web
 * UI calls this on load and before every snapshot. If SNTP later syncs for
 * real, wifi.c's sync_cb calls web_server_note_ntp_sync() to clear the
 * "manual" clockSrc this function sets below. */
static esp_err_t h_time_set(httpd_req_t *req)
{
    char body[48] = {0};
    int len = MIN(req->content_len, (int) sizeof(body) - 1);
    if (len > 0) httpd_req_recv(req, body, len);
    char *ep = strstr(body, "epoch=");
    long long epoch = ep ? atoll(ep + 6) : 0;

    time_t now = time(NULL);
    struct tm tmn;
    localtime_r(&now, &tmn);
    bool already = (tmn.tm_year + 1900 >= 2020);
    bool set = false;

    if (!already && epoch > 1672531200LL) {   /* sane: after 2023-01-01 */
        struct timeval tv = { .tv_sec = (time_t) epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        setenv("TZ", g_settings.timezone, 1);   /* re-apply after clock set so
                                                   DST/offset resolve correctly */
        tzset();
        set = true;
        s_clock_manual = true;
        ESP_LOGI(TAG, "clock set from browser (epoch %lld) — SNTP had not synced", epoch);
    }

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"set\":%s,\"synced\":%s}",
                     set ? "true" : "false", (already || set) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
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

    camera_fb_t *fb = camera_grab();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
        return ESP_OK;
    }
    char path[96] = {0};
    esp_err_t err = storage_save_jpeg(fb->buf, fb->len, path, sizeof(path));
    unsigned bytes = fb->len;
    camera_return(fb);

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

/* Gallery event labels (§3.4): the visit log records each event's first frame
 * + species, so join that to the day's image files to badge event thumbnails
 * with their identification. Only first frames match (one log row per event);
 * follow-up frames stay unlabeled. */
/* Max visit-log rows loaded per gallery day. A busy feeder day at the reference
 * unit runs 600+ captures (and relabel/copy-last APPENDS a row per no-row frame,
 * landing past the old 300 cap), so labels beyond the cap were silently dropped
 * and their images reverted to "unclassified" in the gallery though the label
 * was saved and in the export. Raised well over a realistic day's row count;
 * the array is PSRAM-backed (~150 KB at 1200) since it's too big for internal
 * heap. Match is O(files×labels) but only on an occasional gallery load. */
#define GAL_MAX_LABELS 1200
/* base holds the capture filename ("YYYY-MM-DD_HH-MM-SS-mmm.jpg", 27 chars);
 * it was [16] — sized for the pre-v1.30 "HHMMSS.jpg" names — so every
 * millisecond-era basename truncated to 15 chars and never matched its file,
 * silently blanking all gallery species badges until v1.53. */
/* state: 0 = unclassified (no real species — sentinel "no bird"/"Unidentified"
 * row, or no row at all), 1 = classified by the model (a real species, latin
 * non-empty, not yet human-confirmed), 2 = human-confirmed (corrected set —
 * whether the user accepted the model's guess or typed their own). §3.4/v1.59 */
typedef struct { char base[48]; char sp[72]; uint8_t pct; bool confirmed; uint8_t state; } gal_label_t;

/* One CSV field, in place; advances *p past the comma (or to the line end). */
static char *gal_next_field(char **p)
{
    char *start = *p;
    char *comma = strchr(start, ',');
    if (comma) { *comma = '\0'; *p = comma + 1; }
    else       { char *end = start + strcspn(start, "\r\n"); *end = '\0'; *p = end; }
    return start;
}

/* Fills labels[] (up to GAL_MAX_LABELS) with basename -> localized species for
 * the given capture date, read from that month's visit log; returns the count. */
static int gal_build_labels(const char *date, gal_label_t *labels)
{
    if (!storage_sd_present()) return 0;
    char logpath[64];
    if (strcmp(date, "no-date") == 0)
        strlcpy(logpath, STORAGE_MOUNT_POINT "/log/visits-no-date.csv", sizeof(logpath));
    else   /* monthly file: visits-YYYY-MM.csv, i.e. the date's first 7 chars */
        snprintf(logpath, sizeof(logpath), STORAGE_MOUNT_POINT "/log/visits-%.7s.csv", date);
    FILE *fp = fopen(logpath, "r");
    if (!fp) return 0;

    char match[48];
    snprintf(match, sizeof(match), "/captures/%.20s/", date);

    int count = 0;
    char line[400];   /* past the longest row incl. roi/top3 — a split row's
                         tail would otherwise parse as a bogus extra row */
    bool header = true;
    while (fgets(line, sizeof(line), fp) && count < GAL_MAX_LABELS) {
        if (header) { header = false; continue; }
        if (line[0] == '\0' || line[0] == '\n') continue;
        char *p = line;
        gal_next_field(&p);                        /* timestamp */
        char *species   = gal_next_field(&p);
        char *conf      = gal_next_field(&p);
        gal_next_field(&p);                        /* frames */
        char *first     = gal_next_field(&p);
        char *corrected = gal_next_field(&p);
        char *latin     = gal_next_field(&p);
        if (!species[0] || !first[0]) continue;
        char *base = strstr(first, match);         /* only this day's frames */
        if (!base) continue;
        base += strlen(match);
        if (!base[0] || strchr(base, '/')) continue;
        labels[count].confirmed = corrected[0] != '\0';
        /* State before corrected-wins swap. Seven per-image states (§3.4/v1.76):
         *   4 confirmed no-bird — human verified the frame is empty (corrected ==
         *     "no bird"); a ground-truth negative for the retrain.
         *   5 other/not-a-bird — human tagged a non-bird subject (cat, sheep;
         *     corrected == "other"); exports as an `other/` HARD NEGATIVE distractor,
         *     kept distinct from empty-frame no-bird.
         *   6 unknown/bad-bird — a bird IS present but is unidentifiable or too
         *     blurry to use (corrected == "unknown"); EXCLUDED from the export — a
         *     bird can never be a hard negative, and it has no usable species label.
         *   2 confirmed species — human set a real label (corrected, not a sentinel).
         *   1 classified — model decided a real species (latin present), unverified.
         *   3 no-bird — model's confident "no bird" call, not yet reviewed (often a
         *     missed bird on this domain-mismatched model, so its own review bucket).
         *   0 unclassified — "Unidentified bird"/no-row frames. */
        labels[count].state = (corrected[0] && strcmp(corrected, "no bird") == 0) ? 4
                            : (corrected[0] && strcmp(corrected, "other")   == 0) ? 5
                            : (corrected[0] && strcmp(corrected, "unknown") == 0) ? 6
                            : corrected[0] ? 2
                            : latin[0]      ? 1
                            : strcmp(species, "no bird") == 0 ? 3 : 0;
        if (corrected[0]) species = corrected;   /* user label wins; latin column
                                                    holds its binomial since v1.51 */
        strlcpy(labels[count].base, base, sizeof(labels[count].base));
        species_localize(species, latin, g_settings.lang,
                         labels[count].sp, sizeof(labels[count].sp));
        labels[count].pct = (uint8_t) atoi(conf);
        count++;
    }
    fclose(fp);
    return count;
}

/* GET /api/events?date=YYYY-MM-DD — files of one capture day, each annotated
 * with its species label + confidence when it's a logged event's first frame */
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

    gal_label_t *labels = heap_caps_calloc(GAL_MAX_LABELS, sizeof(gal_label_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int nlabels = labels ? gal_build_labels(date, labels) : 0;

    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *e;
    bool first = true;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG || e->d_name[0] == '.') continue;
        char fpath[176];
        snprintf(fpath, sizeof(fpath), "%s/%.48s", dir, e->d_name);
        struct stat st = {0};
        stat(fpath, &st);
        const char *sp = NULL;
        int pct = 0;
        bool confirmed = false;
        int state = 0;
        for (int i = 0; i < nlabels; i++)
            if (strcmp(labels[i].base, e->d_name) == 0) {
                sp = labels[i].sp; pct = labels[i].pct;
                confirmed = labels[i].confirmed; state = labels[i].state; break;
            }
        char item[256];
        int len;
        if (sp)
            len = snprintf(item, sizeof(item),
                           "%s{\"f\":\"%.48s\",\"s\":%ld,\"sp\":\"%s\",\"pct\":%d,\"c\":%s,\"st\":%d}",
                           first ? "" : ",", e->d_name, (long) st.st_size, sp, pct,
                           confirmed ? "true" : "false", state);
        else
            len = snprintf(item, sizeof(item), "%s{\"f\":\"%.48s\",\"s\":%ld,\"st\":0}",
                           first ? "" : ",", e->d_name, (long) st.st_size);
        httpd_resp_send_chunk(req, item, len);
        first = false;
    }
    closedir(d);
    free(labels);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/labels — species vocabulary for the relabel picker (§3.4/v1.51):
 * the model's Northern-European subset, each as raw common/latin (the values
 * stored on relabel) plus a display name localized to the current language. */
static esp_err_t h_labels(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    int n = classify_label_count();
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!classify_label_region(i)) continue;
        const char *raw = classify_label(i);
        const char *open = strrchr(raw, '(');
        if (!open) continue;                     /* guard classes (background) */
        char latin[64], common[64];
        size_t ll = (size_t) (open - raw);
        while (ll > 0 && raw[ll-1] == ' ') ll--;
        if (ll >= sizeof(latin)) ll = sizeof(latin) - 1;
        memcpy(latin, raw, ll); latin[ll] = '\0';
        const char *cp = strchr(open, ')');
        size_t cl = cp ? (size_t) (cp - (open + 1)) : strlen(open + 1);
        if (cl >= sizeof(common)) cl = sizeof(common) - 1;
        memcpy(common, open + 1, cl); common[cl] = '\0';
        char disp[96];
        species_localize(common, latin, g_settings.lang, disp, sizeof(disp));
        char c_e[80], l_e[80], d_e[112];
        json_escape(c_e, sizeof(c_e), common);
        json_escape(l_e, sizeof(l_e), latin);
        json_escape(d_e, sizeof(d_e), disp);
        char item[300];
        int len = snprintf(item, sizeof(item), "%s{\"c\":\"%s\",\"l\":\"%s\",\"d\":\"%s\"}",
                           first ? "" : ",", c_e, l_e, d_e);
        httpd_resp_send_chunk(req, item, len);
        first = false;
    }
    /* Off-model target species (§3.2.1/§3.2.2): boreal specialists the v1 model
     * can't emit but that we want to hand-label as ground truth for the Nordic
     * retrain. Offered in the relabel picker with a stable binomial so the
     * export keys on it — one class folder per species, immune to common-name
     * typos. Kept in sync with the "inert" boreal entries in species_i18n.c. */
    static const struct { const char *latin; const char *common; } EXTRA_LABELS[] = {
        { "Perisoreus infaustus", "Siberian Jay" },   /* Lavskrike */
    };
    for (size_t e = 0; e < sizeof(EXTRA_LABELS) / sizeof(EXTRA_LABELS[0]); e++) {
        char disp[96];
        species_localize(EXTRA_LABELS[e].common, EXTRA_LABELS[e].latin,
                         g_settings.lang, disp, sizeof(disp));
        char c_e[80], l_e[80], d_e[112];
        json_escape(c_e, sizeof(c_e), EXTRA_LABELS[e].common);
        json_escape(l_e, sizeof(l_e), EXTRA_LABELS[e].latin);
        json_escape(d_e, sizeof(d_e), disp);
        char item[300];
        int len = snprintf(item, sizeof(item), "%s{\"c\":\"%s\",\"l\":\"%s\",\"d\":\"%s\"}",
                           first ? "" : ",", c_e, l_e, d_e);
        httpd_resp_send_chunk(req, item, len);
        first = false;
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/labels/confirmed — every human-verified row across all monthly
 * visit logs, as ground-truth training labels for the §3.2.1 Nordic retrain
 * (v1.57). A row counts as confirmed when its "corrected" column is non-empty
 * (written only by the v1.51 relabel path). Emits the raw stored common name
 * and Latin binomial (language-independent, unlike /api/events' localized
 * label) plus the capture path so the export tool can pull the image:
 *   [{"f":"/captures/DATE/NAME.jpg","c":"Dompap","l":"Pyrrhula pyrrhula",
 *     "ts":"2026-07-10T21:30:08"}, ...]
 * Streamed chunked — the set grows unbounded as the user keeps relabeling. */
static esp_err_t h_labels_confirmed(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    DIR *d = storage_sd_present() ? opendir(STORAGE_MOUNT_POINT "/log") : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type != DT_REG) continue;
            if (strncmp(e->d_name, "visits-", 7) != 0 || !strstr(e->d_name, ".csv"))
                continue;
            char path[64];
            snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/%.40s", e->d_name);
            FILE *fp = fopen(path, "r");
            if (!fp) continue;
            char line[400];
            bool header = true;
            while (fgets(line, sizeof(line), fp)) {
                if (header) { header = false; continue; }
                if (line[0] == '\0' || line[0] == '\n') continue;
                char *p = line;
                char *ts        = gal_next_field(&p);
                gal_next_field(&p);                    /* species (model guess) */
                gal_next_field(&p);                    /* confidence */
                gal_next_field(&p);                    /* frames */
                char *first_fr  = gal_next_field(&p);
                char *corrected = gal_next_field(&p);
                char *latin     = gal_next_field(&p);
                char *roi       = gal_next_field(&p);   /* "x0-y0-x1-y1" or empty (whole frame) */
                if (!corrected[0] || !first_fr[0]) continue;   /* confirmed rows only */
                char f_e[112], c_e[80], l_e[80], t_e[40], r_e[48];
                json_escape(f_e, sizeof(f_e), first_fr);
                json_escape(c_e, sizeof(c_e), corrected);
                json_escape(l_e, sizeof(l_e), latin);
                json_escape(t_e, sizeof(t_e), ts);
                json_escape(r_e, sizeof(r_e), roi);
                char item[400];
                int len = snprintf(item, sizeof(item),
                    "%s{\"f\":\"%s\",\"c\":\"%s\",\"l\":\"%s\",\"ts\":\"%s\",\"roi\":\"%s\"}",
                    first ? "" : ",", f_e, c_e, l_e, t_e, r_e);
                httpd_resp_send_chunk(req, item, len);
                first = false;
            }
            fclose(fp);
        }
        closedir(d);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/relabel (date, f, c[, l]) — set the user-confirmed species on an
 * image's visit row (§3.4/v1.51). `c` is the common name (stored in the
 * "corrected" column), `l` its binomial (stored in "latin"); free-text `c`
 * with empty `l` is allowed. Adds a row if the image has none. */
static esp_err_t h_relabel(httpd_req_t *req)
{
    char body[256] = {0};
    int rlen = MIN(req->content_len, (int) sizeof(body) - 1);
    int got = 0;
    while (got < rlen) {
        int r = httpd_req_recv(req, body + got, rlen - got);
        if (r <= 0) break;
        got += r;
    }
    char date[16] = {0}, file[80] = {0}, common[80] = {0}, latin[80] = {0};
    httpd_query_key_value(body, "date", date, sizeof(date));
    httpd_query_key_value(body, "f",    file, sizeof(file));
    httpd_query_key_value(body, "c",    common, sizeof(common));
    httpd_query_key_value(body, "l",    latin, sizeof(latin));
    url_decode(date); url_decode(file); url_decode(common); url_decode(latin);

    httpd_resp_set_type(req, "application/json");
    if (strlen(date) != 10 || !file[0] || !common[0] ||
        strstr(file, "..") || strchr(file, '/') || strchr(file, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad params");
        return ESP_OK;
    }
    if (storage_relabel(date, file, common, latin) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"relabel failed\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "relabel %s/%s -> %s", date, file, common);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/confirm (date, f) — accept the model's existing classification as
 * the human-confirmed label (§3.4/v1.59), without re-running the model or
 * retyping the species. One-click "yes, the ESP32 got it right." Copies the
 * row's species into corrected; a row with nothing to confirm (no real species,
 * or already confirmed) replies ok:false so the UI can leave the badge as-is. */
static esp_err_t h_confirm(httpd_req_t *req)
{
    char body[160] = {0};
    int rlen = MIN(req->content_len, (int) sizeof(body) - 1);
    int got = 0;
    while (got < rlen) {
        int r = httpd_req_recv(req, body + got, rlen - got);
        if (r <= 0) break;
        got += r;
    }
    char date[16] = {0}, file[80] = {0};
    httpd_query_key_value(body, "date", date, sizeof(date));
    httpd_query_key_value(body, "f",    file, sizeof(file));
    url_decode(date); url_decode(file);

    httpd_resp_set_type(req, "application/json");
    if (strlen(date) != 10 || !file[0] ||
        strstr(file, "..") || strchr(file, '/') || strchr(file, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad params");
        return ESP_OK;
    }
    esp_err_t e = storage_confirm(date, file);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "confirm %s/%s", date, file);
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (e == ESP_ERR_NOT_FOUND) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nothing to confirm\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"confirm failed\"}");
    }
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

/* Defined further down with the settings handlers; used here for the batch
 * delete body. */
static void form_field(const char *body, const char *key, char *out, size_t olen);

/* POST /api/captures/delete — Gallery bulk cleanup (§3.4). Body (urlencoded):
 * date=YYYY-MM-DD and either all=1 (delete the whole day-folder) or
 * files=a.jpg,b.jpg,... (delete just those). Photos only, unless stats=1 is
 * also set (the "wipe day" action), which additionally removes that day's
 * visit-log rows. */
static esp_err_t h_captures_delete_batch(httpd_req_t *req)
{
    int cap = MIN(req->content_len, 16384);
    char *body = calloc(1, cap + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    int rlen = httpd_req_recv(req, body, cap);
    if (rlen <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_OK; }
    body[rlen] = '\0';

    char date[36] = {0}, all[4] = {0}, stats[4] = {0};
    form_field(body, "date=", date, sizeof(date));
    form_field(body, "all=",  all,  sizeof(all));
    form_field(body, "stats=", stats, sizeof(stats));
    bool bad = !date[0] || strstr(date, "..");
    for (const char *c = date; *c && !bad; c++)
        if (!isalnum((unsigned char) *c) && *c != '-') bad = true;
    if (bad) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad date"); return ESP_OK; }

    char dir[112];
    snprintf(dir, sizeof(dir), STORAGE_MOUNT_POINT "/captures/%.36s", date);
    int deleted = 0;

    storage_write_lock();
    if (all[0] == '1') {
        DIR *dd = opendir(dir);
        if (dd) {
            struct dirent *e;
            while ((e = readdir(dd)) != NULL) {
                if (e->d_type != DT_REG) continue;
                char p[176];
                snprintf(p, sizeof(p), "%s/%.48s", dir, e->d_name);
                if (unlink(p) == 0) deleted++;
            }
            closedir(dd);
            rmdir(dir);                 /* drop the now-empty day-folder */
        }
    } else {
        /* files=a.jpg,b.jpg,... — parsed straight from the body so a long
         * multi-select list isn't truncated by a fixed field buffer. */
        char *fl = strstr(body, "files=");
        char *end = fl ? strchr(fl, '&') : NULL;
        for (char *tok = fl ? fl + 6 : NULL; tok && *tok && (!end || tok < end); ) {
            char *comma = strchr(tok, ',');
            char *stop = comma;
            if (end && (!stop || stop > end)) stop = end;
            size_t tl = stop ? (size_t) (stop - tok) : strlen(tok);
            char fname[52];
            if (tl > 0 && tl < sizeof(fname)) {
                memcpy(fname, tok, tl);
                fname[tl] = '\0';
                if (!strchr(fname, '/') && !strstr(fname, "..")) {
                    char p[176];
                    snprintf(p, sizeof(p), "%s/%.48s", dir, fname);
                    if (unlink(p) == 0) deleted++;
                }
            }
            if (!comma || (end && comma >= end)) break;
            tok = comma + 1;
        }
    }
    storage_write_unlock();

    int stats_removed = 0;
    if (stats[0] == '1') stats_removed = storage_reset_stats_day(date);
    free(body);

    ESP_LOGI(TAG, "gallery delete: %d file(s), %d stat row(s) from %s",
             deleted, stats_removed, date);
    char resp[72];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"deleted\":%d,\"statsRemoved\":%d}",
             deleted, stats_removed);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Stats endpoints (FSD §3.4, §6) — aggregated from the visit logs ────── */
static esp_err_t h_stats_daily(httpd_req_t *req)
{
    stats_t *st = calloc(1, sizeof(stats_t));
    if (!st) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    stats_collect(st);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    for (int i = 0; i < st->day_count; i++) {
        char item[64];
        int len = snprintf(item, sizeof(item), "%s{\"d\":\"%s\",\"n\":%u}",
                           i ? "," : "", st->day[i], st->day_n[i]);
        httpd_resp_send_chunk(req, item, len);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    free(st);
    return ESP_OK;
}

static esp_err_t h_stats_species(httpd_req_t *req)
{
    stats_t *st = calloc(1, sizeof(stats_t));
    if (!st) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    stats_collect(st);
    httpd_resp_set_type(req, "application/json");
    /* Object, not a bare array (v1.46): confirmed false positives ("no bird"
     * rows, §3.4) ride alongside the species rows instead of polluting them. */
    char head[48];
    int hl = snprintf(head, sizeof(head), "{\"falsePos\":%lu,\"rows\":[",
                      (unsigned long) st->false_pos);
    httpd_resp_send_chunk(req, head, hl);
    for (int i = 0; i < st->sp_count; i++) {
        char species[80];
        species_localize(st->sp[i], st->sp_latin[i], g_settings.lang,
                          species, sizeof(species));
        /* "s" is the localized display name; "key" is the raw log species
         * value (the CSV join key) the images endpoint matches on. */
        char s_esc[96], k_esc[80];
        json_escape(s_esc, sizeof(s_esc), species);
        json_escape(k_esc, sizeof(k_esc), st->sp[i]);
        char item[256];
        int len = snprintf(item, sizeof(item),
                           "%s{\"s\":\"%s\",\"key\":\"%s\",\"n\":%u,\"first\":\"%s\",\"last\":\"%s\"}",
                           i ? "," : "", s_esc, k_esc, st->sp_n[i],
                           st->sp_first[i], st->sp_last[i]);
        httpd_resp_send_chunk(req, item, len);
    }
    /* Confirmed false positives as their own row (§3.4/v1.50): count +
     * first/last, keyed on the raw "no bird" species so its images list too. */
    if (st->false_pos > 0) {
        char fp[80];
        species_localize("no bird", "", g_settings.lang, fp, sizeof(fp));
        char fp_esc[96];
        json_escape(fp_esc, sizeof(fp_esc), fp);
        char item[224];
        int len = snprintf(item, sizeof(item),
            "%s{\"s\":\"%s\",\"key\":\"no bird\",\"fp\":true,\"n\":%lu,\"first\":\"%s\",\"last\":\"%s\"}",
            st->sp_count ? "," : "", fp_esc, (unsigned long) st->false_pos,
            st->fp_first, st->fp_last);
        httpd_resp_send_chunk(req, item, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    free(st);
    return ESP_OK;
}

/* GET /api/stats/images?sp=<raw species>&limit=N (N<=10) — the last N
 * first_frame images for a species/false-positive row (FSD §3.4/v1.50). */
static esp_err_t h_stats_images(httpd_req_t *req)
{
    char query[160] = {0}, sp[80] = {0}, lim[8] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "sp", sp, sizeof(sp));
    httpd_query_key_value(query, "limit", lim, sizeof(lim));
    url_decode(sp);
    if (!sp[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing sp"); return ESP_OK; }
    int max = lim[0] ? atoi(lim) : 10;
    if (max < 1) max = 1;
    if (max > 10) max = 10;

    stats_img_t *imgs = calloc(max, sizeof(stats_img_t));
    if (!imgs) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    int n = stats_list_images(sp, imgs, max);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    for (int i = 0; i < n; i++) {
        char f_esc[96];
        json_escape(f_esc, sizeof(f_esc), imgs[i].path);
        char item[160];
        int len = snprintf(item, sizeof(item), "%s{\"f\":\"%s\",\"t\":\"%s\"}",
                           i ? "," : "", f_esc, imgs[i].ts);
        httpd_resp_send_chunk(req, item, len);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    free(imgs);
    return ESP_OK;
}

static esp_err_t h_stats_hourly(httpd_req_t *req)
{
    stats_t *st = calloc(1, sizeof(stats_t));
    if (!st) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    stats_collect(st);
    char buf[192] = "[";
    for (int h = 0; h < 24; h++) {
        char n[10];
        snprintf(n, sizeof(n), "%s%u", h ? "," : "", st->hour[h]);
        strlcat(buf, n, sizeof(buf));
    }
    strlcat(buf, "]", sizeof(buf));
    free(st);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/recheck (date=YYYY-MM-DD[&files=a.jpg,b.jpg,...]) — re-run
 * species ID over that day's visit-log rows with the current model/settings
 * (FSD §3.4); `files` narrows it to the rows whose first frame is in the
 * list (Gallery recheck-selected). Asynchronous; GET reports progress.
 * Body is heap-read like captures/delete: a large multi-select list would
 * overflow any sensible stack buffer. */
#define RECHECK_MAX_BODY (8 * 1024)
static esp_err_t h_recheck_post(httpd_req_t *req)
{
    int total = MIN(req->content_len, RECHECK_MAX_BODY);
    char *body = calloc(1, total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    char *dp = strstr(body, "date=");
    char date[16] = {0};
    if (dp) strlcpy(date, dp + 5, sizeof(date));
    char *amp = strchr(date, '&');
    if (amp) *amp = '\0';
    char *files = strstr(body, "files=");
    if (files) {
        files += 6;
        char *end = strchr(files, '&');
        if (end) *end = '\0';
    }
    if (strlen(date) != 10 || date[4] != '-' || date[7] != '-') {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad date");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    if (!classify_available()) {
        free(body);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"no classifier (no model on SD?)\"}");
        return ESP_OK;
    }
    bool started = classify_recheck_start(date, files);
    free(body);
    if (!started) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"a recheck is already running\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "recheck started for %s%s", date, files ? " (selected files)" : "");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_recheck_get(httpd_req_t *req)
{
    bool busy;
    int done, total;
    char date[16];
    classify_recheck_status(&busy, &done, &total, date, sizeof(date));
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"busy\":%s,\"date\":\"%s\",\"done\":%d,\"total\":%d}",
             busy ? "true" : "false", date, done, total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/stats/reset — deletes the visit-log CSVs (FSD §3.4), clearing
 * historic species recognition; saved photos on SD are untouched. */
static esp_err_t h_stats_reset(httpd_req_t *req)
{
    int deleted = storage_reset_stats();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"deleted\":%d}", deleted);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── IP configuration endpoints (FSD §4.5, RemoteStart v1.37/38) ────────── */
static esp_err_t h_ipcfg_get(httpd_req_t *req)
{
    char live_ip[16] = "", live_mask[16] = "", live_gw[16] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(live_ip,   sizeof(live_ip),   IPSTR, IP2STR(&info.ip));
        snprintf(live_mask, sizeof(live_mask), IPSTR, IP2STR(&info.netmask));
        snprintf(live_gw,   sizeof(live_gw),   IPSTR, IP2STR(&info.gw));
    }
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"static\":%s,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
        "\"liveIp\":\"%s\",\"liveMask\":\"%s\",\"liveGw\":\"%s\"}",
        g_ipcfg_static ? "true" : "false",
        g_ipcfg_ip, g_ipcfg_mask, g_ipcfg_gw, g_ipcfg_dns,
        live_ip, live_mask, live_gw);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Extracts one form field (key must include the '='), URL-decoded */
static void form_field(const char *body, const char *key, char *out, size_t olen)
{
    out[0] = '\0';
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if (p == body || p[-1] == '&') break;
        p++;
    }
    if (!p) return;
    p += strlen(key);
    const char *end = strchr(p, '&');
    size_t l = end ? (size_t) (end - p) : strlen(p);
    if (l >= olen) l = olen - 1;
    memcpy(out, p, l);
    out[l] = '\0';
    url_decode(out);
}

static esp_err_t h_ipcfg_save(httpd_req_t *req)
{
    char body[320] = {0};
    int  len = MIN(req->content_len, (int) sizeof(body) - 1);
    httpd_req_recv(req, body, len);

    char mode[12], ip[16], mask[16], gw[16], dns[16];
    form_field(body, "mode=", mode, sizeof(mode));
    form_field(body, "ip=",   ip,   sizeof(ip));
    form_field(body, "mask=", mask, sizeof(mask));
    form_field(body, "gw=",   gw,   sizeof(gw));
    form_field(body, "dns=",  dns,  sizeof(dns));

    bool is_static = (strcmp(mode, "static") == 0);
    if (is_static) {
        /* Server-side IPv4 validation (RemoteStart v1.37): a syntactically
         * bad static IP would still associate to the AP but be unreachable
         * over IP — indistinguishable from a hang. */
        esp_ip4_addr_t a;
        if (esp_netif_str_to_ip4(ip, &a) != ESP_OK ||
            esp_netif_str_to_ip4(mask, &a) != ESP_OK ||
            (gw[0]  && esp_netif_str_to_ip4(gw,  &a) != ESP_OK) ||
            (dns[0] && esp_netif_str_to_ip4(dns, &a) != ESP_OK)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid IPv4 address");
            return ESP_OK;
        }
    }

    g_ipcfg_static = is_static;
    strlcpy(g_ipcfg_ip,   ip,   sizeof(g_ipcfg_ip));
    strlcpy(g_ipcfg_mask, mask, sizeof(g_ipcfg_mask));
    strlcpy(g_ipcfg_gw,   gw,   sizeof(g_ipcfg_gw));
    strlcpy(g_ipcfg_dns,  dns,  sizeof(g_ipcfg_dns));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#16281c;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#7fc98b'>Saved!</h2>"
        "<p>Applying IP configuration and rebooting…</p></div></body></html>");

    g_ipcfg_save_requested = true;
    return ESP_OK;
}

/* ── Settings endpoints (FSD §5) ────────────────────────────────────────── */
static esp_err_t h_settings_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    /* 8x8 detection zone as a 64-char '1'/'0' string (bit c = row c/8, col c%8,
     * row 0 top) — avoids sending a 64-bit mask through JS Number precision. */
    char zone[65];
    for (int c = 0; c < 64; c++)
        zone[c] = (g_settings.detect_zone >> c) & 1ULL ? '1' : '0';
    zone[64] = '\0';
    char buf[600];
    int n = snprintf(buf, sizeof(buf),
        "{\"mode\":%d,\"sens\":%u,\"ccnt\":%u,\"civl\":%u,\"cool\":%u,"
        "\"conf\":%u,\"cap\":%u,\"qual\":%u,\"ir\":%u,\"rot\":%u,\"rfilt\":%u,"
        "\"res\":%u,\"contrast\":%d,\"ae_level\":%d,\"tz\":\"%s\","
        "\"region\":\"%s\",\"ntp\":\"%s\",\"lang\":%u,"
        "\"zone\":\"%s\",\"dzoom\":%u,\"fshut\":%u,\"tta\":%u,\"qtn\":%u,\"models\":[",
        g_settings.mode, g_settings.motion_sensitivity, g_settings.capture_count,
        g_settings.capture_interval_ms, g_settings.cooldown_s,
        g_settings.confidence_pct, g_settings.sd_cap_pct,
        g_settings.stream_quality, g_settings.ir_led_mode, (unsigned) g_settings.rotation,
        (unsigned) g_settings.region_filter,
        (unsigned) g_settings.resolution, (int) g_settings.contrast,
        (int) g_settings.ae_level,
        g_settings.timezone, g_settings.region, g_settings.ntp_server,
        (unsigned) g_settings.lang, zone, (unsigned) g_settings.detect_zoom,
        (unsigned) g_settings.fast_shutter, (unsigned) g_settings.tta,
        (unsigned) g_settings.detect_quarantine_s);
    httpd_resp_send_chunk(req, buf, n);
    /* The region choices are whatever model files sit in /sd/model (§3.2 —
     * users swap regions by dropping a file on the card or POSTing to
     * /model/upload, no reflash). Label/CSV files are filtered out. */
    if (storage_sd_present()) {
        DIR *d = opendir(STORAGE_MOUNT_POINT "/model");
        if (d) {
            struct dirent *e;
            int i = 0;
            while ((e = readdir(d)) != NULL) {
                if (e->d_type != DT_REG) continue;
                const char *dot = strrchr(e->d_name, '.');
                if (!dot || strcasecmp(dot, ".tflite") != 0) continue;
                n = snprintf(buf, sizeof(buf), "%s\"%s\"", i++ ? "," : "", e->d_name);
                httpd_resp_send_chunk(req, buf, n);
            }
            closedir(d);
        }
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Numeric form field, clamped; absent/empty field keeps the current value. */
static long field_num(const char *body, const char *key, long lo, long hi, long cur)
{
    char v[16];
    form_field(body, key, v, sizeof(v));
    if (!v[0]) return cur;
    long n = strtol(v, NULL, 10);
    return n < lo ? lo : n > hi ? hi : n;
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    char body[640] = {0};
    int  len = MIN(req->content_len, (int) sizeof(body) - 1);
    httpd_req_recv(req, body, len);

    char mode[12], tz[48], region[32], ntp[64];
    form_field(body, "mode=",   mode,   sizeof(mode));
    form_field(body, "tz=",     tz,     sizeof(tz));
    form_field(body, "region=", region, sizeof(region));
    form_field(body, "ntp=",    ntp,    sizeof(ntp));

    if (mode[0])
        g_settings.mode = strcmp(mode, "nestbox") == 0 ? MODE_NESTBOX : MODE_FEEDER;
    g_settings.motion_sensitivity  = field_num(body, "sens=", 0,   100,   g_settings.motion_sensitivity);
    g_settings.capture_count       = field_num(body, "ccnt=", 1,   10,    g_settings.capture_count);
    g_settings.capture_interval_ms = field_num(body, "civl=", 250, 10000, g_settings.capture_interval_ms);
    g_settings.cooldown_s          = field_num(body, "cool=", 1,   600,   g_settings.cooldown_s);
    g_settings.confidence_pct      = field_num(body, "conf=", 0,   100,   g_settings.confidence_pct);
    g_settings.sd_cap_pct          = field_num(body, "cap=",  50,  95,    g_settings.sd_cap_pct);
    g_settings.stream_quality      = field_num(body, "qual=", 5,   40,    g_settings.stream_quality);
    g_settings.ir_led_mode         = field_num(body, "ir=",   0,   1,     g_settings.ir_led_mode);
    g_settings.rotation  = (rotation_t) field_num(body, "rot=", 0,  3,    g_settings.rotation);
    g_settings.region_filter       = field_num(body, "rfilt=", 0, 1,     g_settings.region_filter);
    g_settings.resolution          = field_num(body, "res=",  0,   4,    g_settings.resolution);
    g_settings.contrast            = field_num(body, "contrast=", -2, 2, g_settings.contrast);
    g_settings.ae_level            = field_num(body, "ael=", -2, 2, g_settings.ae_level);
    if (tz[0] && !strchr(tz, '"'))
        strlcpy(g_settings.timezone, tz, sizeof(g_settings.timezone));
    /* region becomes a /sd/model/<region> path when §3.2 lands — reject
     * anything that could escape that directory (or break the GET's JSON) */
    if (!strstr(region, "..") && !strchr(region, '/') &&
        !strchr(region, '\\') && !strchr(region, '"'))
        strlcpy(g_settings.region, region, sizeof(g_settings.region));
    if (ntp[0] && !strchr(ntp, '"') && !strchr(ntp, ' '))
        strlcpy(g_settings.ntp_server, ntp, sizeof(g_settings.ntp_server));
    g_settings.lang = (species_lang_t) field_num(body, "lang=", 0, 1, g_settings.lang);

    /* 8x8 detection-zone mask: 64-char '1'/'0' string, bit c = cell (§3.1).
     * Only applied when present and well-formed, so a partial POST (the Live
     * tab's zone save, or any other tab's save) leaves it untouched. */
    char zone[80];
    form_field(body, "zone=", zone, sizeof(zone));
    if (strlen(zone) == 64) {
        uint64_t m = 0;
        bool ok = true;
        for (int c = 0; c < 64; c++) {
            if (zone[c] == '1') m |= (1ULL << c);
            else if (zone[c] != '0') { ok = false; break; }
        }
        if (ok) g_settings.detect_zone = m;
    }
    g_settings.detect_zoom = field_num(body, "dzoom=", 0, 1, g_settings.detect_zoom);
    g_settings.fast_shutter = field_num(body, "fshut=", 0, 1, g_settings.fast_shutter);
    g_settings.tta = field_num(body, "tta=", 0, 1, g_settings.tta);
    g_settings.detect_quarantine_s = field_num(body, "qtn=", 0, 3600, g_settings.detect_quarantine_s);

    settings_save();

    /* Apply live — no reboot (FSD §5) */
    setenv("TZ", g_settings.timezone, 1);
    tzset();
    camera_set_quality(g_settings.stream_quality);   /* no-op without camera */
    camera_set_rotation(g_settings.rotation);        /* no-op without camera */
    camera_set_contrast(g_settings.contrast);        /* no-op without camera */
    camera_set_ae_level(g_settings.ae_level);        /* no-op without camera */
    /* fast_shutter is applied by motion.c's ambient-dark check (FSD v1.38),
     * not here — it only engages when the scene actually reads dark, and
     * forcing it unconditionally on every save is what caused v1.37's
     * daytime overexposure bug */
    /* resolution is applied at camera_init — needs a reboot (FSD §5) */
    wifi_restart_sntp();                             /* no-op before first connect */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /api/settings/export — download every setting as one form-urlencoded
 * blob, byte-identical to the body POST /api/settings accepts. Restoring is
 * just POSTing this file back (the UI does it), so the restore path reuses that
 * handler's full validation/clamping — no second, drift-prone parser. Purpose:
 * survive an NVS wipe (portal reset / boot-button erase) without hand-re-tuning
 * (FSD §5). tz/region/ntp are emitted raw; their only special chars (,./-) pass
 * url_decode untouched. */
static esp_err_t h_settings_export(httpd_req_t *req)
{
    char zone[65];
    for (int c = 0; c < 64; c++)
        zone[c] = (g_settings.detect_zone >> c) & 1ULL ? '1' : '0';
    zone[64] = '\0';
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "mode=%s&sens=%u&ccnt=%u&civl=%u&cool=%u&conf=%u&cap=%u&qual=%u&ir=%u"
        "&rot=%u&rfilt=%u&res=%u&contrast=%d&ael=%d&tz=%s&region=%s&ntp=%s"
        "&lang=%u&zone=%s&dzoom=%u&fshut=%u&tta=%u&qtn=%u",
        g_settings.mode == MODE_FEEDER ? "feeder" : "nestbox",
        g_settings.motion_sensitivity, g_settings.capture_count,
        g_settings.capture_interval_ms, g_settings.cooldown_s,
        g_settings.confidence_pct, g_settings.sd_cap_pct, g_settings.stream_quality,
        g_settings.ir_led_mode, (unsigned) g_settings.rotation,
        (unsigned) g_settings.region_filter, (unsigned) g_settings.resolution,
        (int) g_settings.contrast, (int) g_settings.ae_level,
        g_settings.timezone, g_settings.region, g_settings.ntp_server,
        (unsigned) g_settings.lang, zone, (unsigned) g_settings.detect_zoom,
        (unsigned) g_settings.fast_shutter, (unsigned) g_settings.tta,
        (unsigned) g_settings.detect_quarantine_s);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=birdbox-settings.cfg");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* POST /api/detect — enable/disable motion detection at runtime for
 * maintenance (FSD §5). Body: enabled=0|1 (absent ⇒ keep current). Runtime
 * only — detection always resumes enabled after a reboot. Replies with the
 * resulting state so the caller can reconcile. */
static esp_err_t h_detect(httpd_req_t *req)
{
    char body[32] = {0};
    int  len = MIN(req->content_len, (int) sizeof(body) - 1);
    if (len > 0) httpd_req_recv(req, body, len);
    bool en = field_num(body, "enabled=", 0, 1, motion_detection_enabled() ? 1 : 0);
    motion_set_detection_enabled(en);
    ESP_LOGI(TAG, "motion detection %s via API", en ? "enabled" : "disabled");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, en ? "{\"enabled\":true}" : "{\"enabled\":false}");
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

    char species[80];
    species_localize(classify_last_species(), classify_last_latin(),
                      g_settings.lang, species, sizeof(species));

    char tstr[24]; const char *tsrc;
    device_time(tstr, sizeof(tstr), &tsrc);

    char buf[720];
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"version\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"heap\":%lu,\"uptime\":%lld,\"portal\":%s,\"wifiReconnects\":%lu,"
        "\"sdPresent\":%s,\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,"
        "\"time\":\"%s\",\"clockSrc\":\"%s\","
        "\"motion\":%s,\"detect\":%s,\"quarantineS\":%u,"
        "\"streamUsed\":%d,\"streamMax\":%d,"
        "\"events\":%lu,\"lastEvent\":\"%s\",\"species\":\"%s\"}",
        FIRMWARE_NAME, FIRMWARE_VERSION, ip, rssi, ch,
        (unsigned long) esp_get_free_heap_size(),
        esp_timer_get_time() / 1000000,
        wifi_in_portal_mode() ? "true" : "false",
        (unsigned long) g_wifi_disconnect_count,
        storage_sd_present() ? "true" : "false",
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        tstr, tsrc,
        motion_active() ? "true" : "false",
        motion_detection_enabled() ? "true" : "false",
        (unsigned) motion_quarantine_remaining_s(),
        s_stream_clients, STREAM_MAX_CLIENTS,
        (unsigned long) capture_event_count(),
        capture_last_event_path(),
        classify_last_species()[0] ? species : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /api/motion — tiny, fast-poll signal for the live-view motion border
 * (FSD §3.3). `n` = motion_trigger_count(): a rising edge means a new detection
 * fired, which the live tab flashes as a 1 s red frame border. `a` = an event
 * is capturing right now; `q` = boot-quarantine seconds left. Deliberately
 * separate from /api/status so the live view can poll it ~2 Hz cheaply. */
static esp_err_t h_motion(httpd_req_t *req)
{
    /* `c` = 8x8 trigger-cell mask as a 64-char '1'/'0' string, bit c = cell
     * (row c/8, col c%8) — same layout as the settings "zone" mask, so the live
     * overlay reuses the grid geometry. Marks the winning cluster of the last
     * detection so the UI can highlight exactly which boxes fired. */
    uint64_t cells = motion_trigger_cells();
    char cbuf[65];
    for (int c = 0; c < 64; c++) cbuf[c] = (cells >> c) & 1ULL ? '1' : '0';
    cbuf[64] = '\0';
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"n\":%lu,\"a\":%s,\"q\":%u,\"c\":\"%s\"}",
             (unsigned long) motion_trigger_count(),
             motion_active() ? "true" : "false",
             (unsigned) motion_quarantine_remaining_s(), cbuf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Why the chip last rebooted (FSD §5): "panic"/"wdt" point at a firmware
 * fault, "brownout" at power, "software" is OTA/reboot-button — first
 * question to answer when a unit restarts unexpectedly in the field. */
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external-pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        default:                return "unknown";
    }
}

/* GET /api/sysinfo — Debug tab: heap/low-water/uptime/reconnects, WiFi link,
 * SD card health, camera sensor status, last-inference timing (FSD §5, §6) */
static esp_err_t h_sysinfo(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_ap_record_t ap = {0};
    int rssi = 0, ch = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { rssi = ap.rssi; ch = ap.primary; }
    char apssid[70];
    json_escape(apssid, sizeof(apssid), (char *) ap.ssid);
    char tstr[24]; const char *tsrc;
    device_time(tstr, sizeof(tstr), &tsrc);

    uint64_t sd_total, sd_free;
    storage_get_info(&sd_total, &sd_free);
    char card[24];
    storage_get_card_name(card, sizeof(card));

    int64_t now_us = esp_timer_get_time();

    char buf[960];
    int n = snprintf(buf, sizeof(buf),
        "{\"heap\":%lu,\"heapMin\":%lu,\"heapMinAgo\":%lld,\"uptime\":%lld,"
        "\"resetReason\":\"%s\","
        "\"wifiDisc\":%lu,\"wifiDiscAgo\":%lld,\"mac\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"apSsid\":\"%s\",\"time\":\"%s\",\"clockSrc\":\"%s\","
        "\"sdPresent\":%s,\"sdCard\":\"%s\",\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,"
        "\"sdWriteOk\":%s,"
        "\"camPresent\":%s,\"camPid\":%d,\"camRes\":\"%s\",\"camQuality\":%u,"
        "\"camRecoveries\":%lu,\"camRecoveryAgo\":%d,\"camFault\":%s,"
        "\"socTempC\":%.1f,\"motionTriggers\":%lu,"
        "\"lastInferenceMs\":%ld,\"clsModel\":\"%s\",\"clsLabels\":%d,\"clsRegion\":%d}",
        (unsigned long) esp_get_free_heap_size(),
        (unsigned long) g_heap_min,
        (long long) ((now_us - g_heap_min_ts_us) / 1000000),
        now_us / 1000000,
        reset_reason_str(),
        (unsigned long) g_wifi_disconnect_count,
        g_wifi_last_disc_ts_us ? (long long) ((now_us - g_wifi_last_disc_ts_us) / 1000000) : -1,
        mac_str, rssi, ch, apssid, tstr, tsrc,
        storage_sd_present() ? "true" : "false", card,
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        storage_last_write_ok() ? "true" : "false",
        camera_available() ? "true" : "false", camera_get_pid(),
        camera_framesize_str(), g_settings.stream_quality,
        (unsigned long) camera_recovery_count(), camera_last_recovery_ago_s(),
        camera_fault() ? "true" : "false",
        soc_temp_c(), (unsigned long) motion_trigger_count(),
        (long) classify_last_duration_ms(),
        classify_model_name(), classify_label_count(), classify_region_matches());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* POST /ota/upload — raw .bin upload (FSD §8), RemoteStart pattern: writes
 * to the inactive OTA slot and only switches the boot partition after a
 * full, verified write, so a partial upload just fails rather than leaving
 * a half-written slot as the next boot target. Bounded httpd_req_recv()
 * retries on transient timeouts (v1.5 lesson) so one dropped packet doesn't
 * abort a multi-hundred-KB upload. The actual brick-proofing is the
 * bootloader's rollback (sdkconfig's CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE +
 * main.c's esp_ota_mark_app_valid_cancel_rollback()): if this new image
 * never reaches that call, the next boot reverts to the current slot. */
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }
    if (req->content_len <= 0 || (size_t) req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or oversized upload");
        return ESP_OK;
    }

    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    char buf[1024];
    int  remaining = req->content_len;
    bool ok = true;
    int  timeout_retries = 0;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, MIN(remaining, (int) sizeof(buf)));
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries < 5) continue;
            ok = false;
            break;
        }
        timeout_retries = 0;
        if (esp_ota_write(ota, buf, got) != ESP_OK) { ok = false; break; }
        remaining -= got;
    }

    if (!ok || esp_ota_end(ota) != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA upload failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
        return ESP_OK;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "OTA upload complete — rebooting into new image");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* Localize one raw model label ("Scientific name (Common Name)") to the
 * current display language. The classifier only splits the *winning* label
 * into species/latin; the top-3 arrive as raw English strings, so parse the
 * "Latin (Common)" shape here and run it through the same localizer, else the
 * top-3 breakdown ignores the language setting. */
static void classify_localize_label(const char *raw, char *out, size_t outsz)
{
    const char *op = strstr(raw, " (");
    const char *cp = op ? strrchr(raw, ')') : NULL;
    if (op && cp && cp > op + 2) {
        char latin[64], common[64];
        size_t ll = (size_t) (op - raw);
        if (ll >= sizeof(latin)) ll = sizeof(latin) - 1;
        memcpy(latin, raw, ll);
        latin[ll] = '\0';
        size_t cl = (size_t) (cp - (op + 2));
        if (cl >= sizeof(common)) cl = sizeof(common) - 1;
        memcpy(common, op + 2, cl);
        common[cl] = '\0';
        species_localize(common, latin, g_settings.lang, out, outsz);
    } else {
        snprintf(out, outsz, "%s", raw);   /* no parseable binomial */
    }
}

/* Shared JSON reply for the classify endpoints: localized species name plus
 * the top-3 labels (localized to the display language) / percentages / timing. */
/* `saved`: the gallery identify path persisted this result as the row's
 * confirmed label (§3.4/v1.58) — the UI flips the thumbnail badge to confirmed.
 * Always false for the posted-image classify (no visit row to write). */
static esp_err_t classify_send_json(httpd_req_t *req, const classify_result_t *r,
                                    bool saved)
{
    char species[80];
    species_localize(r->species, r->latin, g_settings.lang, species, sizeof(species));

    char l0[96], l1[96], l2[96];
    classify_localize_label(r->top_label[0], l0, sizeof(l0));
    classify_localize_label(r->top_label[1], l1, sizeof(l1));
    classify_localize_label(r->top_label[2], l2, sizeof(l2));

    char buf[800];
    snprintf(buf, sizeof(buf),
        "{\"species\":\"%s\",\"confidence\":%u,\"durationMs\":%ld,\"saved\":%s,\"top3\":["
        "{\"label\":\"%s\",\"pct\":%u},{\"label\":\"%s\",\"pct\":%u},"
        "{\"label\":\"%s\",\"pct\":%u}]}",
        species, r->confidence_pct, (long) r->duration_ms, saved ? "true" : "false",
        l0, r->top_pct[0], l1, r->top_pct[1], l2, r->top_pct[2]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/classify — classify a posted JPEG right now (FSD §3.2/§6).
 * Lets users sanity-check their model without waiting for a bird, and is
 * how the classifier is verified end-to-end. Blocks this httpd thread for
 * the full decode + inference (~seconds) — acceptable for a manual op. */
#define CLASSIFY_MAX_BODY (300 * 1024)
static esp_err_t h_classify_run(httpd_req_t *req)
{
    if (!classify_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"no classifier (no model on SD?)\"}");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > CLASSIFY_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or oversized JPEG");
        return ESP_OK;
    }
    uint8_t *body = heap_caps_malloc(req->content_len,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int remaining = req->content_len, timeout_retries = 0;
    bool ok = true;
    while (remaining > 0) {
        int got = httpd_req_recv(req, (char *) body + (req->content_len - remaining),
                                 remaining);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries < 5) continue;
            ok = false;
            break;
        }
        timeout_retries = 0;
        remaining -= got;
    }

    classify_result_t r;
    esp_err_t err = ok ? classify_run_sync(body, req->content_len, &r) : ESP_FAIL;
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "classification failed");
        return ESP_OK;
    }
    return classify_send_json(req, &r, false);   /* posted image → no row to save into */
}

/* GET /api/classify-file?date=<day>&f=<file> — classify a JPEG already on the
 * SD card (FSD §3.2/§6). Lets the Gallery re-run species ID on a saved photo
 * without re-uploading it over WiFi: the device reads the file itself. Blocks
 * this httpd thread for the full decode + inference (~seconds) like /api/classify. */
static esp_err_t h_classify_file(httpd_req_t *req)
{
    if (!classify_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"no classifier (no model on SD?)\"}");
        return ESP_OK;
    }
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD card");
        return ESP_OK;
    }
    char query[128] = {0}, date[24] = {0}, file[80] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "date", date, sizeof(date));
    httpd_query_key_value(query, "f", file, sizeof(file));
    url_decode(date);
    url_decode(file);
    if (!date[0] || !file[0] ||
        strstr(date, "..") || strchr(date, '/') || strchr(date, '\\') ||
        strstr(file, "..") || strchr(file, '/') || strchr(file, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    char path[160];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/captures/%.23s/%.79s", date, file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > CLASSIFY_MAX_BODY) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or oversized JPEG");
        return ESP_OK;
    }
    uint8_t *body = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t rd = fread(body, 1, sz, f);
    fclose(f);
    if (rd != (size_t) sz) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
        return ESP_OK;
    }

    classify_result_t r;
    esp_err_t err = classify_run_sync(body, sz, &r);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "classification failed");
        return ESP_OK;
    }
    /* Gallery identify persists its result as the row's confirmed label
     * (§3.4/v1.58): a non-empty latin means a real species was decided (above
     * threshold) — write it via the same path the ✎ relabel button uses, so
     * the record changes and it feeds the retrain export (v1.57). An
     * under-threshold "Unidentified"/"no bird" result writes nothing (latin is
     * empty), so an inconclusive identify never wipes a good row. */
    bool saved = false;
    if (r.latin[0] && storage_relabel(date, file, r.species, r.latin) == ESP_OK) {
        saved = true;
        ESP_LOGI(TAG, "identify saved %s/%s -> %s", date, file, r.species);
    }
    return classify_send_json(req, &r, saved);
}

/* POST /model/upload?name=<file> — write a model/labels file into /sd/model
 * (FSD §3.2 model swap without pulling the card). Held under the §7 write
 * lock for the whole stream, so a motion capture during the ~seconds-long
 * upload waits rather than interleaving. Takes effect on next boot. */
static esp_err_t h_model_upload(httpd_req_t *req)
{
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SD card");
        return ESP_OK;
    }
    char query[80] = {0}, name[48] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "name", name, sizeof(name));
    url_decode(name);
    const char *dot = strrchr(name, '.');
    if (!name[0] || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\') ||
        !dot || (strcasecmp(dot, ".tflite") && strcasecmp(dot, ".txt") &&
                 strcasecmp(dot, ".csv"))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "name must be a plain .tflite/.txt/.csv filename");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 6 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or oversized upload");
        return ESP_OK;
    }

    char path[96];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/model/%.48s", name);

    storage_write_lock();
    FILE *f = fopen(path, "wb");
    if (!f) {
        storage_write_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }
    char buf[2048];
    int remaining = req->content_len, timeout_retries = 0;
    bool ok = true;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, MIN(remaining, (int) sizeof(buf)));
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries < 5) continue;
            ok = false;
            break;
        }
        timeout_retries = 0;
        if (fwrite(buf, 1, got, f) != (size_t) got) { ok = false; break; }
        remaining -= got;
    }
    fclose(f);
    if (!ok) unlink(path);   /* no half-written model left as next boot's pick */
    storage_write_unlock();

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "model file uploaded: %s (%d bytes)", path, req->content_len);
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"file\":\"%s\",\"bytes\":%d,\"note\":\"reboot to load\"}",
             name, req->content_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /model/delete?name=<file> — remove a model/labels file from /sd/model,
 * the symmetric counterpart to /model/upload (candidate models accrete during
 * the §3.2.2 retrain iteration and need cleanup without pulling the card).
 * Refuses to delete any file belonging to the currently loaded model — its
 * .tflite or its .txt, matched by basename stem — so you can't blow away what's
 * running. */
static esp_err_t h_model_delete(httpd_req_t *req)
{
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SD card");
        return ESP_OK;
    }
    char query[80] = {0}, name[48] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "name", name, sizeof(name));
    url_decode(name);
    const char *dot = strrchr(name, '.');
    if (!name[0] || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\') ||
        !dot || (strcasecmp(dot, ".tflite") && strcasecmp(dot, ".txt") &&
                 strcasecmp(dot, ".csv"))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "name must be a plain .tflite/.txt/.csv filename");
        return ESP_OK;
    }
    /* Guard the loaded model set (its .tflite and .txt share a basename stem). */
    const char *active = classify_model_name();          /* "" if none loaded */
    if (active[0]) {
        char astem[48];
        strlcpy(astem, active, sizeof(astem));
        char *ad = strrchr(astem, '.');
        if (ad) *ad = '\0';
        size_t nstem = (size_t) (dot - name);
        if (strlen(astem) == nstem && strncmp(name, astem, nstem) == 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"error\":\"that model is in use — select another and reboot first\"}");
            return ESP_OK;
        }
    }
    char path[96];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/model/%.48s", name);
    struct stat st;
    if (stat(path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such model file");
        return ESP_OK;
    }
    storage_write_lock();
    int rc = unlink(path);
    storage_write_unlock();
    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "model file deleted: %s", path);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"deleted\":\"%s\"}", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* Pins already spoken for by the camera/SD/flash-PSRAM bus or fixed to a
 * boot-strapping/USB/UART role — off limits for the raw GPIO debug toggle
 * below, since driving one of these can wedge the camera, SD card or the
 * board itself rather than just (not) lighting an LED. */
static bool gpio_debug_reserved(int n)
{
    static const int reserved[] = {
        CAM_PIN_PWDN, CAM_PIN_RESET, CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC,
        CAM_PIN_Y9, CAM_PIN_Y8, CAM_PIN_Y7, CAM_PIN_Y6, CAM_PIN_Y5, CAM_PIN_Y4,
        CAM_PIN_Y3, CAM_PIN_Y2, CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK,
#ifdef SD_PIN_CLK
        SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0,
#endif
        SD_PIN_CS,
        0, 3, 19, 20, 43, 44, 45, 46,      /* strapping / USB-JTAG / UART0 console */
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,   /* octal flash+PSRAM bus */
    };
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (reserved[i] == n) return true;
    return false;
}

/* POST /api/debug/gpio — body "num=<n>&level=<0|1>". Drives a raw GPIO pin
 * high/low so an unlabeled onboard LED can be traced to its pin number by
 * eye. PIN_IR_LED (GPIO48 on the reference unit) is a WS2812, not a plain
 * LED — a raw level can clock garbage into its shift register and a
 * follow-up low just latches that garbage rather than clearing it (observed
 * live: it stayed lit after level=0) — so that pin is routed through the
 * shared illum.c driver instead, which sends a real bit-timed frame. Other
 * pins revert to input on the next reboot; the illuminator keeps whatever
 * state it was last set to (matches its normal auto-mode behaviour). */
static esp_err_t h_debug_gpio(httpd_req_t *req)
{
    char body[64] = {0};
    int  len = MIN(req->content_len, (int) sizeof(body) - 1);
    if (len > 0) httpd_req_recv(req, body, len);

    long num   = field_num(body, "num=",   0, 48, -1);
    long level = field_num(body, "level=", 0, 1,  -1);
    if (num < 0 || level < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "num and level are required");
        return ESP_OK;
    }

#if PIN_IR_LED >= 0
    if (num == PIN_IR_LED) {
        if (!illum_available()) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "illuminator driver init failed");
            return ESP_OK;
        }
        illum_set((bool) level);
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }
#endif

    if (gpio_debug_reserved((int) num)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "pin is reserved by camera/SD/flash — refused");
        return ESP_OK;
    }

    gpio_reset_pin((gpio_num_t) num);
    gpio_set_direction((gpio_num_t) num, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t) num, (int) level);

    httpd_resp_sendstr(req, "OK");
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
    cfg.max_uri_handlers = 48;     /* headroom above route count (35 as of v1.51) — an
                                      exact-fit cap silently drops later registrations,
                                      404ing whichever routes land last (RemoteStart v1.27;
                                      hit again at v1.51 when the count crossed 32) */
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
        { .uri = "/api/portal-status", .method = HTTP_GET, .handler = h_portal_status },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = h_scan       },
        { .uri = "/api/wificfg", .method = HTTP_GET,  .handler = h_wificfg    },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = h_status     },
        { .uri = "/api/motion",  .method = HTTP_GET,  .handler = h_motion     },
        { .uri = "/api/capture", .method = HTTP_POST, .handler = h_capture    },
        { .uri = "/api/detect",  .method = HTTP_POST, .handler = h_detect     },
        { .uri = "/api/time",    .method = HTTP_POST, .handler = h_time_set   },
        { .uri = "/api/days",    .method = HTTP_GET,  .handler = h_days       },
        { .uri = "/api/events",  .method = HTTP_GET,  .handler = h_events     },
        { .uri = "/api/labels",  .method = HTTP_GET,  .handler = h_labels     },
        { .uri = "/api/labels/confirmed", .method = HTTP_GET, .handler = h_labels_confirmed },
        { .uri = "/api/relabel", .method = HTTP_POST, .handler = h_relabel    },
        { .uri = "/api/confirm", .method = HTTP_POST, .handler = h_confirm    },
        { .uri = "/api/stats/daily",   .method = HTTP_GET, .handler = h_stats_daily   },
        { .uri = "/api/stats/species", .method = HTTP_GET, .handler = h_stats_species },
        { .uri = "/api/stats/hourly",  .method = HTTP_GET, .handler = h_stats_hourly  },
        { .uri = "/api/stats/images",  .method = HTTP_GET, .handler = h_stats_images  },
        { .uri = "/api/stats/reset",   .method = HTTP_POST, .handler = h_stats_reset  },
        { .uri = "/api/recheck",       .method = HTTP_POST, .handler = h_recheck_post },
        { .uri = "/api/recheck",       .method = HTTP_GET,  .handler = h_recheck_get  },
        { .uri = "/api/ipconfig",      .method = HTTP_GET,  .handler = h_ipcfg_get  },
        { .uri = "/api/ipconfig/save", .method = HTTP_POST, .handler = h_ipcfg_save },
        { .uri = "/api/settings",      .method = HTTP_GET,  .handler = h_settings_get  },
        { .uri = "/api/settings",      .method = HTTP_POST, .handler = h_settings_post },
        { .uri = "/api/settings/export", .method = HTTP_GET, .handler = h_settings_export },
        { .uri = "/api/sysinfo",       .method = HTTP_GET,  .handler = h_sysinfo    },
        { .uri = "/api/captures/delete", .method = HTTP_POST, .handler = h_captures_delete_batch },
        { .uri = "/captures/*",  .method = HTTP_GET,  .handler = h_captures_file },
        { .uri = "/captures/*",  .method = HTTP_DELETE, .handler = h_captures_delete },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = h_reboot     },
        { .uri = "/api/debug/gpio", .method = HTTP_POST, .handler = h_debug_gpio },
        { .uri = "/ota/upload",  .method = HTTP_POST, .handler = h_ota_upload },
        { .uri = "/api/classify",  .method = HTTP_POST, .handler = h_classify_run },
        { .uri = "/api/classify-file", .method = HTTP_GET, .handler = h_classify_file },
        { .uri = "/model/upload",  .method = HTTP_POST, .handler = h_model_upload },
        { .uri = "/model/delete",  .method = HTTP_POST, .handler = h_model_delete },
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
