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
#include "cloud.h"
#include "inat.h"
#include "logo_data.h"   /* BIRD_LOGO: Dompap header logo (data URI) */
#include "cities_data.h" /* CITIES_JS: capital-city dropdown for the iNat geo hint */
#include "species_i18n.h"
#include "target_species.h"
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
#if CONFIG_HEAP_TRACING
#include "esp_heap_trace.h"   /* leak hunt for the outbound-HTTPS leak (v2.50) */
#endif
#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
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
".logo{width:1.5em;height:1.5em;border-radius:50%;object-fit:cover;"
"vertical-align:-.35em;margin-right:.15em}"
"</style></head>"
"<body><div class='box'>"
"<h2><img class='logo' src='" BIRD_LOGO "' alt=''> " FIRMWARE_NAME " WiFi Setup</h2>"
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
".logo{width:1.5em;height:1.5em;border-radius:50%;object-fit:cover;vertical-align:-.35em;"
"margin-right:.15em;box-shadow:0 1px 3px rgba(0,0,0,.35)}"
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
/* CLASSIFYING (v2.54): red = frames being captured, yellow = iNat scoring them.
 * Same slot as DETECTING since the two never overlap — capture finishes before
 * the job is queued — so the badge reads as one state machine, not two lamps. */
".clsbadge{position:absolute;top:8px;left:8px;z-index:3;display:none;"
"align-items:center;gap:6px;background:rgba(212,160,23,.92);color:#1b1500;"
"font-size:.72rem;font-weight:700;letter-spacing:.05em;padding:4px 9px;"
"border-radius:4px}"
".clsbadge.on{display:inline-flex}"
".clsbadge .dot{width:9px;height:9px;border-radius:50%;background:#1b1500;"
"animation:detpulse 1s ease-in-out infinite}"
".liveWrap.cls{outline:3px solid rgba(212,160,23,.9);outline-offset:-3px}"
".liveWrap.mot{outline:5px solid #ff2f2f;outline-offset:-5px;"   /* 1s per-trigger motion flash */
"box-shadow:inset 0 0 18px rgba(255,47,47,.6)}"
"@keyframes detpulse{0%,100%{opacity:1}50%{opacity:.25}}"
/* Live-view species overlays (v2.32): CURRENT (bottom-left) = the most recent
 * event's outcome, ephemeral; LAST IDENTIFIED (bottom-right) = the last real
 * species ID, sticky. Both share .livesp; #livesp2 flips to the right edge. */
".livesp{position:absolute;left:8px;bottom:8px;z-index:3;display:none;"
"align-items:center;gap:6px;max-width:46%;background:rgba(16,22,18,.78);"
"color:#eafaef;font-size:.9rem;font-weight:600;padding:5px 11px;border-radius:6px;"
"pointer-events:none;box-shadow:0 1px 4px rgba(0,0,0,.4)}"
"#livesp2{left:auto;right:8px}"
".livesp.on{display:inline-flex}"
".livesp.uncl{opacity:.7;font-style:italic;font-weight:500}"
".livesp .splbl{font-size:.6rem;letter-spacing:.05em;text-transform:uppercase;opacity:.55;"
"font-weight:600;font-style:normal}"
".livesp .spc{font-weight:500;color:#a8d8bb;font-size:.82rem}"
/* Last ID is a link to that event's best frame — inherit the badge's look so it
 * reads as a badge, with a subtle underline as the only affordance (v2.53). */
/* The badge itself is pointer-events:none so it never blocks the video, which
 * also made the anchor inert — re-enable input on the LINK alone (v2.54). */
".livesp .splink{color:inherit;text-decoration:none;display:inline-flex;align-items:center;"
"gap:.35em;pointer-events:auto;cursor:pointer}"
".livesp .splink:hover{text-decoration:underline}"
"#livesp2:has(.splink){pointer-events:auto}"
".livesp .splogo{display:inline-block;width:1.15em;height:1.15em;border-radius:50%;"
"background:url('" BIRD_LOGO "') center/cover;flex:0 0 auto}"
".livesp.pop{animation:livesppop .55s ease}"
"@keyframes livesppop{0%{transform:scale(.8)}55%{transform:scale(1.07)}100%{transform:scale(1)}}"
".pausebadge{position:absolute;top:8px;right:8px;z-index:3;display:none;"
"align-items:center;gap:6px;background:rgba(180,140,40,.92);color:#1a1205;"
"font-size:.72rem;font-weight:700;letter-spacing:.05em;padding:4px 9px;"
"border-radius:4px}"
/* SD-write-failure banner: full-width red bar across the top of the live frame,
 * unmissable — a wedged card silently drops captures (FSD v2.14). */
".sdbadge{position:absolute;top:0;left:0;right:0;z-index:5;display:none;"
"background:rgba(200,40,40,.96);color:#fff;font-size:.74rem;font-weight:700;"
"text-align:center;padding:5px 8px;letter-spacing:.03em}"
".sdbadge.on{display:block}"
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
".gbar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin-bottom:10px}"
/* Action buttons in a responsive grid so every one is the same size and they
 * wrap into tidy rows instead of shrinking to their text on a narrow window. */
".gacts{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:8px;margin-bottom:12px}"
".gacts button.act{width:100%;margin:0;white-space:nowrap;text-align:center}"
/* Tally as an aligned label/value table instead of a run-on wrapped sentence. */
".gtally{margin:0 0 12px}"
".gtallytab{border-collapse:collapse;font-size:.8rem}"
".gtallytab td{padding:1px 0}"
".gtallytab .gtl{color:#9ab;padding-right:16px}"
".gtallytab .gtv{text-align:right;color:#cfe;font-weight:600;font-variant-numeric:tabular-nums}"
".gthint{font-size:.74rem;color:#9ab;margin-top:6px;max-width:32rem}"
".gprog{display:flex;align-items:center;gap:10px;margin:0 0 12px}"
".gprogbar{flex:1;max-width:340px;height:12px;background:#12261a;border:1px solid #2a4d34;border-radius:6px;overflow:hidden}"
".gprogfill{height:100%;width:0;background:#3f8a4f;transition:width .12s linear}"
".gprogtxt{font-size:.82rem;color:#7fc98b;white-space:nowrap}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}"
".gitem{background:#1e3826;border-radius:8px;overflow:hidden;position:relative}"
".gitem img{width:100%;display:block;aspect-ratio:16/9;object-fit:cover;background:#000}"
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
".glabel.gprop{background:rgba(44,46,52,.7);color:#8a9aa6;font-style:italic}"  /* propagated: inherits its visit's label, not independently classified */
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
"<div class='hdr'><h1><img class='logo' src='" BIRD_LOGO "' alt=''> " FIRMWARE_NAME "</h1>"
"<span class='v'>v" FIRMWARE_VERSION "</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"livep\",this)'>Live</button>"
"<button class='tab' onclick='show(\"galleryp\",this)'>Gallery</button>"
"<button class='tab' onclick='show(\"statsp\",this)'>Stats</button>"
"<button class='tab' onclick='show(\"setp\",this)'>Settings</button>"
"<button class='tab' onclick='show(\"maintp\",this)'>Maint</button>"
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
"<div class='clsbadge' id='clsbadge'><span class='dot'></span>CLASSIFYING</div>"
"<div class='pausebadge' id='pausebadge'>&#9208; DETECTION OFF</div>"
"<img class='live' id='live' src='/stream' alt='live stream'"
" onerror='liveErr()' onload='liveOk()'>"
"<div class='livemsg' id='livemsg'></div>"
"<div class='zone' id='zone'></div>"
"<div class='motgrid' id='motgrid'></div>"
"<div class='livesp' id='livesp'></div>"
"<div class='livesp' id='livesp2'></div>"
"<div class='sdbadge' id='sdbadge'>&#9888; SD WRITE FAILING &mdash; captures are not being saved</div>"
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
/* Species sub-filter: appears only in Confirmed-bird mode, lists the distinct
 * confirmed species present on the loaded day so you can narrow to just one. */
"<select id='gspecf' class='gflt' style='display:none;margin-left:6px' onchange='applyFilter()'></select>"
"<span class='sts' id='gselc' style='margin:0;color:#7fc98b'></span></div>"
"<div class='gacts'>"
"<button class='act' onclick='gSelAll()'>&#9745; Select all</button>"
"<button class='act' style='background:#3f8a4f' onclick='gConfirmSel()'>&#10004; Confirm selected</button>"
"<button class='act' style='background:#3f6a8a' onclick='gSpecToggle()'>&#128278; Set species&#9662;</button>"
"<button class='act' onclick='gDelSel()'>&#10060; Delete selected</button>"
"<button class='act' onclick='gRecheck()'>&#128269; Recheck (day)</button>"
"<button class='act' onclick='gRecheckSel()'>&#128269; Recheck selected</button>"
"</div>"
"<div class='gbar' id='gspecbar' style='display:none'>"
"<span class='sts' style='margin:0'>Set species on selected:</span>"
"<input list='speclist' id='gspecin' class='rlin' placeholder='species&hellip;'>"
"<button class='act' style='margin:0;background:#3f6a8a' onclick='gSpecApply()'>Apply</button>"
"<button class='act' style='margin:0;background:#555' onclick='gSpecToggle()'>Cancel</button>"
"<span class='sts' id='gspecsts' style='margin:0'></span></div>"
"<div class='gprog' id='gprog' style='display:none'>"
"<div class='gprogbar'><div class='gprogfill' id='gprogfill'></div></div>"
"<span class='gprogtxt' id='gprogtxt'></span></div>"
"<div class='gtally' id='gsts'></div>"
"<div class='grid' id='grid'></div>"
"<datalist id='speclist'></datalist>"
"</div>"
"<div id='statsp' class='pane'>"
"<div class='gacts' style='margin-bottom:12px'>"
"<button class='act' id='stgToday' style='opacity:1' onclick='setStatScope(\"day\")'>Today</button>"
"<button class='act' id='stgAll' style='opacity:.5' onclick='setStatScope(\"all\")'>All time</button>"
"</div>"
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
"<p class='sts' style='margin-top:2px'>Also sends iNaturalist a native-res crop centred on the "
"bird (the tight changed-pixel box) and keeps whichever scores higher &mdash; whole or crop &mdash; "
"so a small or off-centre bird that under-scores whole can still be identified. Costs a second "
"iNaturalist call per frame. Saved photos are unaffected. Pick the detection zone on the Live tab.</p>"
"<label class='wl'><input type='checkbox' id='stFshut'> Reduce motion blur (fast shutter)</label>"
"<p class='sts' style='margin-top:2px'>When the scene reads dark, fixes the camera's exposure "
"short instead of letting it lengthen &mdash; the main cause of a blurry close/fast-moving bird "
"at dusk or indoors. Only engages in low light (same brightness check as the illuminator), so "
"normal daylight shots are unaffected; low-light shots come out darker/noisier in exchange for "
"less blur.</p>"
"<h3 class='sh'>Species Identification</h3>"
"<label class='wl'>Confidence threshold (%)</label>"
"<input class='wi' type='number' min='15' max='95' id='stConf'>"
"<label class='wl'>Species set</label>"
"<select class='wi' id='stRfilt'>"
"<option value='0'>All species (no region filter)</option>"
"<option value='1'>Norway only</option></select>"
"<p class='sts' style='margin-top:2px'>Norway restricts IDs to the regional species in the "
"allowlist, dropping any out-of-region result: iNaturalist's geographically-impossible top hit "
"(e.g. Pica hudsonia, the American magpie) is rejected and the best in-region candidate wins "
"instead (e.g. Pica pica).</p>"
"<h3 class='sh'>iNaturalist online ID (primary, free)</h3>"
"<label class='wl'><input type='checkbox' id='stInatCv'> Identify new birds with iNaturalist first</label>"
"<p class='sts' style='margin-top:2px'>Sends each new motion event to iNaturalist's <b>online</b> Computer "
"Vision model (the one that powers the iNat app) &mdash; current and region-aware. It's the <b>primary</b> "
"classifier; Claude/Gemini (if configured) are the fallback. <b>It's free</b> (no per-call fee). Needs a "
"token or a session cookie (below).</p>"
"<label class='wl'>iNaturalist username</label>"
"<input class='wi' type='text' id='stIuser' autocomplete='off' placeholder='(not set)'>"
"<label class='wl'>Password</label>"
"<input class='wi' type='password' id='stIpass' autocomplete='new-password' placeholder='(not set)'>"
"<p class='sts' style='margin-top:2px'><b>Recommended &mdash; set this and you never paste anything again.</b> "
"iNaturalist has no permanent API key: its tokens die after ~24&nbsp;h. With your login stored, the box signs "
"in by itself (the way a browser does), mints its own session and keeps its token fresh indefinitely &mdash; "
"press <b>Log in now</b> after saving to test it. <b>Security:</b> the password is stored on the device in "
"plain text (a flash dump would reveal it) and is never shown back or exported; use the fallback fields below "
"if you'd rather not store it.</p>"
"<label class='wl'>Location (nearest capital)</label>"
"<select class='wi' id='stLoc'></select>"
"<p class='sts' style='margin-top:2px'>iNaturalist's model is <b>much</b> more accurate with a location "
"&mdash; it favours species that actually occur near you. Pick the capital nearest your feeder. Without "
"it, iNat runs vision-only and can suggest birds from the wrong continent at low confidence.</p>"
"<button class='act' style='margin-left:0' id='stInatTest' onclick='inatTest()'>&#128268; Test token</button>"
"<button class='act' style='margin-left:0' id='stInatRefresh' onclick='inatRefresh()'>&#128260; Refresh token now</button>"
"<button class='act' style='margin-left:0' id='stInatLogin' onclick='inatLogin()'>&#128273; Log in now</button>"
"<button class='act' style='display:none' id='stIkeyClr' onclick='ikeyClear()'>&#128465; Forget token</button>"
"<div class='sts' id='stInatSts' style='margin-top:4px'></div>"
"<p class='sts' style='margin-top:10px'><b>Advanced &mdash; manual fallback.</b> Only needed if the automatic "
"login stops working (iNaturalist can change its login page at any time). Either field below gets "
"identification running again without reflashing the box; both are kept out of the settings export.</p>"
"<label class='wl'>Session cookie (mints tokens for weeks)</label>"
"<input class='wi' type='password' id='stIsess' autocomplete='off' placeholder='(not set)'>"
"<p class='sts' style='margin-top:2px'>Paste <b>_inaturalist_session</b> and the box re-fetches a fresh 24&nbsp;h "
"token from it. Get it after logging in at inaturalist.org: DevTools &rarr; Application &rarr; Cookies &rarr; "
"inaturalist.org &rarr; copy the value. Lasts weeks. This is also what the automatic login produces for "
"itself.</p>"
"<label class='wl'>iNaturalist API token (JWT, ~24 h)</label>"
"<input class='wi' type='password' id='stIkey' autocomplete='off' placeholder='(not set)'>"
"<p class='sts' style='margin-top:2px'>The shortest-lived option &mdash; a hand-pasted token stops working the "
"next day. From <b>inaturalist.org/users/api_token</b>; the whole "
"<code>{&quot;api_token&quot;:&quot;...&quot;}</code> JSON is fine, the box extracts it. Leave blank to keep "
"the current token.</p>"
"<h3 class='sh'>Cloud Identification (secondary classifier)</h3>"
"<p class='sts' style='margin-top:2px'>When iNaturalist can't identify a new motion event, "
"send its best photo to a cloud vision model and use that answer instead &mdash; iNaturalist stays "
"primary and handles the events it's confident about, so the cloud is only consulted for the hard ones. "
"If a call fails (no internet, bad key, no credit), the iNaturalist result (or <i>Unidentified</i>) stands, "
"so nothing is ever left worse off. Needs your own API key below, and each escalated event costs a "
"fraction of a US cent to a couple of cents against it &mdash; a busy feeder can run to a few dollars "
"a day, so leave this off unless you are actively gathering labels. <b>Photos leave the device only "
"while a provider is selected.</b> Regardless of the selection, the Gallery's &#10024; button asks "
"the cloud about a single photo whenever a key is set.</p>"
"<label class='wl'>Active provider</label>"
"<select class='wi' id='stCloud'>"
"<option value='0'>Off &mdash; iNaturalist only</option>"
"<option value='1'>Anthropic Claude (Opus 4.8)</option>"
"<option value='2'>Google Gemini (2.5 Flash)</option>"
"</select>"
"<p class='sts' style='margin-top:2px'>Only one runs at a time. Both keys can be stored; the "
"selector picks which one identifies new events.</p>"
"<label class='wl'>Anthropic API key</label>"
"<input class='wi' type='password' id='stCkey' autocomplete='off' placeholder='(not set)'>"
"<p class='sts' style='margin-top:2px'>Create one at console.anthropic.com/settings/keys. Stored "
"on the device and sent only to api.anthropic.com; it is never shown again once saved, and is "
"left out of the settings export/backup file. Leave blank to keep the current key.</p>"
"<button class='act' style='margin-left:0' id='stCldTest' "
"onclick='cldTest(1)'>&#128268; Test Claude</button>"
"<button class='act' style='display:none' id='stCkeyClr' "
"onclick='ckeyClear(1)'>&#128465; Forget Claude key</button>"
"<div class='sts' id='stCldSts' style='margin-top:4px'></div>"
"<label class='wl'>Google Gemini API key</label>"
"<input class='wi' type='password' id='stGkey' autocomplete='off' placeholder='(not set)'>"
"<p class='sts' style='margin-top:2px'>Create one at aistudio.google.com/apikey. Stored "
"on the device and sent only to generativelanguage.googleapis.com; it is never shown again once "
"saved, and is left out of the settings export/backup file. Leave blank to keep the current key.</p>"
"<label class='wl'>Gemini model</label>"
"<input class='wi' type='text' id='stGmodel' autocomplete='off' spellcheck='false' "
"placeholder='gemini-flash-lite-latest (default)'>"
"<p class='sts' style='margin-top:2px'>Which model to call. Model availability changes with your "
"Google account and as generations retire, so if a call 404s (&ldquo;not available to new "
"users&rdquo; / &ldquo;no longer available&rdquo;), press <b>Test Gemini</b> below to list the "
"model ids your key can use, then paste one here and Save &mdash; no reflash. Blank = default "
"(gemini-flash-lite-latest). Lowercase letters, digits, dot and hyphen only.</p>"
"<button class='act' style='margin-left:0' id='stGemTest' "
"onclick='cldTest(2)'>&#128268; Test Gemini</button>"
"<button class='act' style='display:none' id='stGkeyClr' "
"onclick='ckeyClear(2)'>&#128465; Forget Gemini key</button>"
"<div class='sts' id='stGemSts' style='margin-top:4px'></div>"
"<h3 class='sh'>Periodic iNat re-scan (optional)</h3>"
"<label class='wl'><input type='checkbox' id='stInat'> Re-scan unidentified frames with the iNaturalist model</label>"
"<p class='sts' style='margin-top:2px'>Every so often, temporarily loads the iNaturalist model from the "
"SD card and re-runs it (whole-frame) on frames the on-device model and Claude left unidentified, "
"restricted to your 30 target species &mdash; a background booster for the hard cases. "
"<b>Off by default:</b> iNaturalist is a weak match for this camera's images, and each pass swaps the "
"model in memory, so live identification pauses for the length of the batch. Requires "
"<code>inat-birds-v1.tflite</code> on the card.</p>"
"<label class='wl'>Re-scan interval (minutes)</label>"
"<input class='wi' type='number' min='5' max='1440' id='stInatv'>"
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
"<p class='sts' id='stResWarn' style='display:none;color:#e0a030;margin-top:2px'></p>"
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
"<div id='maintp' class='pane'>"
"<h3 class='sh' style='margin-top:0'>Day maintenance</h3>"
"<p class='sts'>Destructive, whole-day operations moved out of the Gallery so they "
"can't be hit by accident while labelling. Pick a day, then delete its photos or "
"wipe it entirely. Cannot be undone.</p>"
"<div class='gbar'><span class='sts' style='margin:0'>Day</span>"
"<select id='mday'></select>"
"<button class='act' style='margin:0' onclick='mtLoad()'>&#8635; Refresh</button>"
"<button class='act' style='margin:0;background:#8a3f3f' onclick='mtDelAll()'>&#128465; Delete all photos</button>"
"<button class='act' style='margin:0;background:#9e3030' onclick='mtWipeDay()'>&#9888; Wipe day (photos+stats)</button>"
"<span class='sts' id='mtSts' style='margin:0'></span></div>"
"<p class='sts'><b>Delete all photos</b> removes every image for the day but leaves "
"the statistics/visit log intact. <b>Wipe day</b> also removes that day&#8217;s rows "
"from the statistics/visit log. To clear all statistics, use the Stats tab.</p>"
"<h3 class='sh'>Backfill ROI (bird box)</h3>"
"<p class='sts'>Confirmed birds captured before always-on ROI logging have no motion "
"box, so ROI-crop training can&#8217;t use them. Pick a day, load the birds still missing "
"a box, then <b>click the bird&#8217;s centre</b> on each image (adjust the box with the "
"size slider) and Save. Newer captures record the box automatically.</p>"
"<div class='gbar'><span class='sts' style='margin:0'>Day</span>"
"<select id='rbday'></select>"
"<button class='act' style='margin:0' onclick='rbLoad()'>Load missing-ROI birds</button>"
"<span class='sts' id='rbSts' style='margin:0'></span></div>"
"<div id='rbEdit' style='display:none;margin-top:8px'>"
"<div id='rbWrap' onclick='rbClick(event)' style='position:relative;display:inline-block;"
"max-width:100%;cursor:crosshair;line-height:0'>"
"<img id='rbImg' onload='rbDraw()' style='max-width:100%;display:block'>"
"<div id='rbBox' style='position:absolute;border:2px solid #7fc98b;"
"box-shadow:0 0 0 9999px rgba(0,0,0,.4);display:none;pointer-events:none'></div></div>"
"<div class='gbar' style='margin-top:6px'><span class='sts' style='margin:0'>Box size</span>"
"<input type='range' id='rbSize' min='15' max='90' value='55' oninput='rbDraw()'>"
"<button class='act' style='margin:0' onclick='rbSkip()'>Skip</button>"
"<button class='act' style='margin:0;background:#3f7a4a' onclick='rbSave()'>Save &amp; next</button>"
"<span class='sts' id='rbProg' style='margin:0'></span></div></div>"
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
"<h3 class='sh'>Flash from GitHub release</h3>"
"<p class='sts'>Pull a published release straight from GitHub &mdash; the device "
"downloads and flashes it itself over HTTPS (no PC needed). Repo-locked to this "
"project's releases; same dual-partition rollback as a manual upload.</p>"
"<div class='gbar'><select id='ghRel'><option>Load releases&hellip;</option></select>"
"<button class='act' style='margin:0' onclick='ghLoad()'>&#8635; Reload</button>"
"<button class='act' style='margin:0;background:#3f8a4f' onclick='ghFlash()'>&#11015;&#65039; Download &amp; flash</button></div>"
"<div class='gprog' id='ghProg' style='display:none'>"
"<div class='gprogbar'><div class='gprogfill' id='ghFill'></div></div>"
"<span class='gprogtxt' id='ghTxt'></span></div>"
"<div class='sts' id='ghSts'></div>"
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
"if(id==='maintp')mtLoad();"
"if(id==='dbgp')loadDebug();"
"if(id==='wifip'){ipLoad();wfCfg();}"
"if(id==='otap')ghLoad();}"
"function tick(){fetch('/api/status').then(r=>r.json()).then(s=>{"
"var t=(s.time?('\\uD83D\\uDD52 '+s.time+' ('+s.clockSrc+')'):'\\uD83D\\uDD52 clock not set')+' | '"
"+s.ip+' | RSSI '+s.rssi+' dBm | heap '+Math.round(s.heap/1024)+' KB | up '+s.uptime+' s'"
"+' | SD '+(s.sdPresent?s.sdFreeMB+' MB free':'none')"
"+' | motion '+(s.motion?'ACTIVE':'idle')+' | events '+s.events"
"+(s.quarantineS>0?(' | \\u23F3 quarantine '+s.quarantineS+'s'):'');"
"if(s.lastEvent)t+=' | last: <a href=\"'+s.lastEvent+'\">'+s.lastEvent.split('/').pop()+'</a>';"
"if(s.species)t+=' ('+s.species+')';"
"document.getElementById('sts').innerHTML=t;"
/* Species overlay in the live view: the last event's species + confidence with a
 * Dompap avatar. Shown (and pops) on a NEW event — keyed to the events count, so
 * two events of the same species still re-trigger — then AUTO-HIDES after
 * SP_TTL_MS (3 min) so it doesn't linger forever. dataset.v guards re-rendering. */
"var spC=$g('livesp'),spL=$g('livesp2');"
/* CURRENT (left): the last EVENT's classification RESULT. Keys on clsSeq, which
 * increments when the ASYNC result lands (seconds after capture) — the old
 * events-count trigger fired at capture time, before iNat replied, so a bird that
 * classified showed a stale "unclassified" and never refreshed. A classified
 * species shows for 1 min and is NOT wiped by an intervening unclassified event;
 * "unclassified" shows only when no species is live. A NEW EVENT blanks it
 * immediately (see below). LAST IDENTIFIED (right): the last real species ID
 * (s.species is sticky on the device), no TTL. (v2.40, retimed v2.53) */
"if(spC){"
/* A NEW EVENT blanks the badge at once (v2.53): while the next bird is being
 * captured and scored, the previous bird's name is stale — better to show
 * nothing for those seconds than the wrong species. evStart increments at
 * CAPTURE time, clsSeq when the result lands, so this clears then refills. */
"if(s.evStart!==g_evStart){g_evStart=s.evStart;"
"spC.classList.remove('on');spC.dataset.v='';g_spAt=0;}"
"if(s.clsSeq!==g_clsSeq){g_clsSeq=s.clsSeq;"
"if(s.spLive&&s.species){var hc='<span class=splbl>current</span><span class=splogo></span>'+esc(s.species)+(s.spConf?' <span class=spc>'+s.spConf+'%<\\/span>':'');"
"spC.classList.remove('uncl');spC.dataset.v=hc;spC.innerHTML=hc;g_spAt=Date.now();"
"spC.classList.add('on');spC.classList.remove('pop');void spC.offsetWidth;spC.classList.add('pop');}"
"else if(!spC.classList.contains('on')||!g_spAt||Date.now()-g_spAt>60000){"
"spC.classList.add('uncl');spC.dataset.v='u';spC.innerHTML='<span class=splbl>current</span>unclassified';g_spAt=Date.now();spC.classList.add('on');}}"
"else if(g_spAt&&Date.now()-g_spAt>60000)spC.classList.remove('on');"
"}"
/* LAST ID links to the frame that scored that ID's PEAK confidence (spFile) —
 * the event's best image, not merely its first frame (v2.53). No spFile (e.g.
 * nothing identified since boot) falls back to plain text. */
"if(spL){if(s.species){var hb='<span class=splbl>last id</span>'+esc(s.species)+(s.spConf?' <span class=spc>'+s.spConf+'%<\\/span>':'');"
"var hl=s.spFile?'<a class=splink href=\"'+esc(s.spFile)+'\" target=_blank rel=noopener>'+hb+'<\\/a>':hb;"
"if(spL.dataset.v!==hl){spL.dataset.v=hl;spL.innerHTML=hl;}spL.classList.add('on');}"
"else spL.classList.remove('on');}"
"var sb=$g('sdbadge');if(sb)sb.classList.toggle('on',s.sdWriteOk===false);"
/* Two-stage state (v2.54): RED "detecting" while frames are being captured,
 * then YELLOW "classifying" until iNat's verdict lands. Capture always finishes
 * before the job is queued, so they never overlap — motion wins if they ever do,
 * keeping exactly one lamp lit. */
"var db=$g('detbadge'),cb=$g('clsbadge'),lw=$g('liveWrap');"
"var busyCls=!!s.clsBusy&&!s.motion;"
"if(db)db.classList.toggle('on',!!s.motion);"
"if(cb)cb.classList.toggle('on',busyCls);"
"if(lw){lw.classList.toggle('det',!!s.motion);lw.classList.toggle('cls',busyCls);}"
"if(s.quarantineS>0){var pb=$g('pausebadge');"
"if(pb){pb.innerHTML='\\u23F3 BOOT QUARANTINE '+s.quarantineS+'s';pb.classList.add('on');}}"
"else if(typeof s.detect!=='undefined')detApply(!!s.detect);"
"var lm=$g('livemsg');"
"if(lm&&lm.classList.contains('on')&&s.streamUsed<s.streamMax)liveRetry();"   /* slot freed — reconnect */
"}).catch(()=>{});}tick();setInterval(tick,2000);"
/* Fast per-trigger motion border: poll the tiny /api/motion ~2Hz while the live
 * tab is open; a rising trigger count flashes a red frame border for 1s. */
"var g_motN=-1,g_clsSeq=-1,g_evStart=-1,g_spAt=0;"   /* g_clsSeq: last classification-result seq shown in the Current badge; g_evStart: last capture-time event seq (blanks the badge); g_spAt: its shown-at (1-min TTL) */
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
"g_tally=null;document.getElementById('gsts').textContent='no captures yet';}});}"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\"/g,'&quot;');}"
"function loadDay(){var d=$g('day').value;if(!d)return;"
"fetch('/api/events?date='+d).then(r=>r.json()).then(a=>{"
"a.sort((x,y)=>y.f.localeCompare(x.f));"
/* Display-propagation: a motion event labels only its trigger frame, so a
 * visit's follow-up frames land 'unclassified'. For DISPLAY only, carry the
 * visit's label forward onto consecutive unclassified frames within GPGAP
 * seconds, so a bird's burst reads as the bird instead of a wall of
 * 'unclassified'. The real st (and the visit log / stats) is never changed;
 * o.prop just marks a tile whose label is inherited, not independently found. */
"(function(){var GPGAP=15;function gts(f){var m=f.match(/(\\d{4})-(\\d\\d)-(\\d\\d)_(\\d\\d)-(\\d\\d)-(\\d\\d)-(\\d{3})/);return m?Date.UTC(+m[1],+m[2]-1,+m[3],+m[4],+m[5],+m[6])/1000+ +m[7]/1000:null;}"
"var an=0,asp='',apc=0,at=null;a.slice().reverse().forEach(function(o){var t=gts(o.f),st=o.st||0;"
"if(st>=1&&st<=4){an=st;asp=o.sp||'';apc=o.pct;at=t;}"
"else if(st===0){if(an&&at!=null&&t!=null&&(t-at)<=GPGAP){o.prop=1;o.pst=an;o.psp=asp;o.ppct=apc;at=t;}else{an=0;at=null;}}"
"else{an=0;at=null;}});})();"
"var nC=a.filter(o=>o.st===1).length,nF=a.filter(o=>o.st===2).length,nB=a.filter(o=>o.st===3).length,nX=a.filter(o=>o.st===4).length,nO=a.filter(o=>o.st===5).length,nU=a.filter(o=>o.st===6).length;"
"g_tally={n:a.length,nF:nF,nC:nC,nB:nB,nX:nX,nO:nO,nU:nU};"
"var g=$g('grid');g.innerHTML='';"
"a.forEach(o=>{var p='/captures/'+d+'/'+o.f;var st=o.st||0;var nm=o.sp||'';var pc=' '+(o.pct||0)+'%';"
"var prop=(st===0&&!!o.prop);var pnm=o.psp||'';"
"var div=document.createElement('div');div.className='gitem';div.dataset.st=st;div.dataset.sp=nm;div.dataset.f=o.f;div.dataset.pct=(o.pct==null?-1:o.pct);"
"div.dataset.prop=prop?'1':'';div.dataset.psp=pnm;div.dataset.pst=(o.pst||0);"
"var bcl=st===2?'gconf':st===1?'gcls':st===3?'gnb':st===4?'gnbc':st===5?'goth':st===6?'gunk':'gunc';"
"var btxt=st===2?('\\u2713 '+esc(nm)):st===4?('\\u2713 '+esc(nm||'no bird')):st===5?'\\u2713 not a bird':st===6?'\\u2713 unknown bird':st===1?(esc(nm)+pc):st===3?(esc(nm)+pc):(nm?esc(nm):'unclassified');"
"var bttl=st===2?(esc(nm)+' \\u2013 confirmed'):st===4?(esc(nm||'no bird')+' \\u2013 no bird (confirmed)'):st===5?'other / not a bird \\u2013 hard negative':st===6?'unknown / bad bird \\u2013 excluded from training':st===1?(esc(nm)+pc+' \\u2013 classified'):st===3?(esc(nm)+pc+' \\u2013 no bird (model \\u2013 review)'):'unclassified';"
"if(prop){bcl='gprop';btxt='\\u2248 '+esc(pnm);bttl=esc(pnm)+' \\u2013 same visit (inherited label, not independently classified)';}"
"var sc=o.src||0;"   /* provenance: 1 nordic, 2 Claude, 3 iNat, 4 Gemini */
"var snm=sc===1?'nordic model':sc===2?'Claude':sc===3?'iNat':sc===4?'Gemini':sc===5?'iNaturalist CV':'';"
"var slt=sc===1?'N':sc===2?'C':sc===3?'I':sc===4?'G':sc===5?'V':'';"
"var sbg=sc?(' <span style=\"opacity:.6;font-size:9px\" title=\"labelled by '+snm+'\">['+slt+']</span>'):'';"
"var bdg='<div class=\"glabel '+bcl+'\" title=\"'+bttl+'\">'+btxt+sbg+'</div>';"
"var cfb=st===1?('<button class=\"gidbtn gcfb\" title=\"confirm this species\" data-d=\"'+d+'\" data-f=\"'+esc(o.f)+'\" onclick=\"confirmSp(this)\">\\u2713</button>')"
":st===3?('<button class=\"gidbtn gcfb\" title=\"confirm: no bird\" data-d=\"'+d+'\" data-f=\"'+esc(o.f)+'\" onclick=\"confirmNoBird(this)\">\\u2713</button>'):'';"
"div.innerHTML=bdg+'<input type=\"checkbox\" class=\"gchk\" data-f=\"'+esc(o.f)+'\" "
"title=\"shift-click to select the whole range\" "
"onclick=\"gChkClick(event,this)\" onchange=\"gSelSync(this)\">"
"<a href=\"'+p+'\" target=\"_blank\"><img loading=\"lazy\" src=\"'+p+'\"></a>"
"<div class=\"gmeta\"><span>'+o.f+'</span>"
"<span><button class=\"gidbtn\" title=\"open full image (new tab)\" "
"onclick=\"openFull(\\''+p+'\\')\">&#128065;</button>"
"<button class=\"gidbtn\" title=\"identify bird (primary: iNaturalist if on, else on-device model)\" "
"onclick=\"idBird(this,\\''+d+'\\',\\''+o.f+'\\')\">&#128269;</button>"
"<button class=\"gidbtn\" title=\"identify bird with the cloud classifier\" "
"onclick=\"cldBird(this,\\''+d+'\\',\\''+o.f+'\\')\">&#10024;</button>'+cfb+'"
"<button class=\"gidbtn\" title=\"set/correct species\" data-d=\"'+d+'\" "
"data-f=\"'+esc(o.f)+'\" data-sp=\"'+esc(nm)+'\" onclick=\"reLabel(this)\">&#9998;</button>"
"<button class=\"gidbtn gcpl\" data-d=\"'+d+'\" data-f=\"'+esc(o.f)+'\" "
"onclick=\"copyLast(this)\"'+(g_lastSp?' title=\"copy last: '+esc(g_lastSp.d)+'\"':' title=\"copy last species (label one first)\" disabled')+'>&#128203;</button>"
"<button title=\"delete\" onclick=\"del(\\''+p+'\\')\">&#10060;</button></span></div>';"
"g.appendChild(div);});gSelSync();fillSpecFilter();applyFilter();});}"
"var g_gfilter='all',g_tally=null,g_nbBound=false,g_conf=17,g_lastSp=null,g_selAnchor=null;"
/* Aligned label/value table + optional hint line, instead of a run-on sentence
 * that wrapped one word per line in the toolbar column. */
"function renderTally(vis){var t=g_tally;var el=$g('gsts');if(!t){el.textContent='';return;}"
"var rows=[['captures',t.n],['confirmed',t.nF],['classified',t.nC],['no bird',t.nB]];"
"if(t.nX)rows.push(['conf. no-bird',t.nX]);"
"if(t.nO)rows.push(['other',t.nO]);"
"if(t.nU)rows.push(['unknown',t.nU]);"
"if(g_gfilter!=='all')rows.push(['shown',vis]);"
"var h='<table class=\"gtallytab\">';"
"rows.forEach(function(r){h+='<tr><td class=\"gtl\">'+r[0]+'</td><td class=\"gtv\">'+r[1]+'</td></tr>';});"
"h+='</table>';"
"var hint=g_gfilter==='unc'?'double-click a tile = no bird':"
"g_gfilter==='near'?('top guess '+(g_conf-5)+'\\u2013'+(g_conf-1)+'% \\u2013 just under the '+g_conf+'% threshold \\u00b7 double-click = no bird'):'';"
"if(hint)h+='<div class=\"gthint\">'+hint+'</div>';"
"el.innerHTML=h;}"
/* Populate the confirmed-species sub-filter from the confirmed (st===2) tiles
 * on the loaded day; keep the current pick if it's still present. */
"function fillSpecFilter(){var sf=$g('gspecf');var cur=sf.value;var set={};"
"[...document.querySelectorAll('#grid .gitem')].forEach(function(it){"
"if(+(it.dataset.st||0)===2){var s=it.dataset.sp||'';if(s)set[s]=1;}});"
"var names=Object.keys(set).sort(function(a,b){return a.localeCompare(b);});"
"sf.innerHTML='<option value=\"\">All species</option>';"
"names.forEach(function(s){var o=document.createElement('option');o.value=s;o.textContent=s;sf.appendChild(o);});"
"sf.value=(cur&&names.indexOf(cur)>=0)?cur:'';}"
"function setFilter(v){g_gfilter=v;$g('grid').classList.toggle('nbmode',v==='unc'||v==='near');"
"var sf=$g('gspecf');if(v==='conf'){fillSpecFilter();sf.style.display='';}else{sf.style.display='none';sf.value='';}"
"applyFilter();}"
"function applyFilter(){var items=[...document.querySelectorAll('#grid .gitem')],vis=0,lo=g_conf-5;"
"var spv=g_gfilter==='conf'?$g('gspecf').value:'';"
"items.forEach(function(it){var st=+(it.dataset.st||0),p=+(it.dataset.pct);"
"var prop=it.dataset.prop==='1',pst=+(it.dataset.pst||0),esp=prop?(it.dataset.psp||''):it.dataset.sp;"
"var near=st===0&&!prop&&p>=lo&&p<g_conf;"   /* top-1 within 5% below the ID threshold */
"var show=g_gfilter==='all'||(g_gfilter==='cls'&&(st===1||(prop&&pst===1)))||(g_gfilter==='conf'&&(st===2||(prop&&pst===2))&&(spv===''||esp===spv))||(g_gfilter==='nb'&&(st===3||(prop&&pst===3)))||(g_gfilter==='cnb'&&(st===4||(prop&&pst===4)))||(g_gfilter==='oth'&&st===5)||(g_gfilter==='unk'&&st===6)||(g_gfilter==='unc'&&st===0&&!prop)||(g_gfilter==='near'&&near);"
"it.style.display=show?'':'none';"
"if(show){vis++;}else{var cb=it.querySelector('.gchk');if(cb&&cb.checked){cb.checked=false;it.classList.remove('sel');}}});"
"renderTally(vis);gSelSync();}"
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
/* Both identify buttons render identically — same reply shape, same
 * confirmed-badge flip on save — so they differ only by endpoint. */
"function idBird(btn,d,f){idRun(btn,d,f,'/api/id-primary');}"
"function cldBird(btn,d,f){idRun(btn,d,f,'/api/cloud-file');}"
"function idRun(btn,d,f,url){var it=btn.closest('.gitem');"
"var out=it.querySelector('.gid');"
"if(!out){out=document.createElement('div');out.className='gid';it.appendChild(out);}"
"out.textContent='\\u2026 identifying';btn.disabled=true;"
"fetch(url+'?date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f))"
".then(r=>r.json()).then(o=>{btn.disabled=false;"
"if(o.error){out.textContent=o.error;return;}"
"var t=(o.top3||[]).filter(x=>x.pct>0).map(x=>esc(x.label)+' '+x.pct+'%').join(', ');"
"out.innerHTML='<b>'+esc(o.species||'?')+'</b> '+(o.confidence||0)+'%'"
"+(o.saved?' \\u2013 saved \\u2713':'')+(t?'<span class=gidt>'+t+'</span>':'');"
"if(o.saved){var bdg=it.querySelector('.glabel');"
"if(!bdg){bdg=document.createElement('div');bdg.className='glabel';it.insertBefore(bdg,it.firstChild);}"
"bdg.className='glabel gconf';bdg.textContent='\\u2713 '+(o.species||'');"
"bdg.title=(o.species||'')+' \\u2013 confirmed';it.dataset.st=2;it.dataset.sp=(o.species||'');"
"var cb=it.querySelector('.gcfb');if(cb)cb.remove();"
/* A confident identify (cloud ✨ or on-device 🔍) that produced a real species
 * seeds the 📋 copy-last fast-path, so a strong Gemini/Claude label on one frame
 * can be one-click applied to its sibling frames. Gated on latin (empty ⇒
 * no-bird/Unidentified, not a species to copy). */
"if(o.latin){g_lastSp={c:o.common||o.species,l:o.latin,d:o.species};refreshCopyLast();}"
"applyFilter();}})"
".catch(()=>{btn.disabled=false;out.textContent='identify failed';});}"
/* Enable + retitle every 📋 copy-last button in place once g_lastSp is set,
 * without a full day reload (idRun updates a single tile; the buttons were
 * rendered disabled at load). */
"function refreshCopyLast(){if(!g_lastSp)return;"
"document.querySelectorAll('.gcpl').forEach(function(b){b.disabled=false;"
"b.title='copy last: '+g_lastSp.d;});}"
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
/* Shift-click range select: shift-clicking a checkbox sets every visible tile
 * between the last-clicked (anchor) and this one to this box's new state — the
 * fast way to multiselect a whole burst/region. Runs on click (which carries
 * shiftKey and the already-toggled .checked, unlike change). g_selAnchor is
 * updated on every click; a stale anchor after a reload just yields index -1
 * (no range), so nothing breaks. */
"function gChkClick(e,cb){"
"if(e.shiftKey&&g_selAnchor&&g_selAnchor!==cb){"
"var v=gVisChecks(),i=v.indexOf(g_selAnchor),j=v.indexOf(cb);"
"if(i>=0&&j>=0){var a=Math.min(i,j),b=Math.max(i,j),on=cb.checked;"
"for(var k=a;k<=b;k++){v[k].checked=on;v[k].closest('.gitem').classList.toggle('sel',on);}"
"gSelSync();}}"
"g_selAnchor=cb;}"
/* Sequential POST runner — the ESP32 httpd has only a handful of worker
 * sockets, so bulk label ops go one at a time (never a fan-out of fetches).
 * bodyFn builds the form body per file; cb(okCount) runs when all are done. */
"function gProg(label,done,total){var p=$g('gprog');"
"if(total<=0){p.style.display='none';return;}"
"p.style.display='';var pct=Math.round(done*100/total);"
"$g('gprogfill').style.width=pct+'%';"
"$g('gprogtxt').textContent=label+' \\u2013 '+done+' / '+total+' ('+pct+'%)';}"
"function gSeq(url,fs,bodyFn,cb,label){var i=0,ok=0;label=label||'Working';"
"gProg(label,0,fs.length);(function nxt(){"
"if(i>=fs.length){setTimeout(function(){$g('gprog').style.display='none';},500);cb(ok);return;}"
"fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:bodyFn(fs[i])})"
".then(r=>r.json()).then(function(o){if(o&&o.ok)ok++;}).catch(()=>{}).then(function(){i++;gProg(label,i,fs.length);nxt();});})();}"
"function gConfirmSel(){var fs=gChecks().filter(c=>c.checked).map(c=>c.dataset.f);"
"if(!fs.length){alert('No images selected');return;}var d=$g('day').value;"
"if(!confirm('Confirm the model\\u2019s species on '+fs.length+' selected image(s)? "
"Tiles the model classified become confirmed; anything with nothing to confirm is skipped.'))return;"
"gSeq('/api/confirm',fs,function(f){return 'date='+encodeURIComponent(d)+'&f='+encodeURIComponent(f);},"
"function(ok){$g('gselc').textContent=ok+' confirmed';loadDay();},'Confirming');}"
"function gSpecToggle(){var b=$g('gspecbar');if(b.style.display!=='none'){b.style.display='none';return;}"
"ensureSpecs(function(){b.style.display='';$g('gspecsts').textContent='';"
"var inp=$g('gspecin');inp.value='';inp.focus();"
"inp.onkeydown=function(e){if(e.key==='Enter')gSpecApply();};});}"
"function gSpecApply(){var fs=gChecks().filter(c=>c.checked).map(c=>c.dataset.f);"
"if(!fs.length){$g('gspecsts').textContent='no images selected';return;}"
"var v=$g('gspecin').value.trim();if(!v){$g('gspecsts').textContent='enter a species';return;}"
"var d=$g('day').value,m=g_specMap[v]||{c:v,l:''};"
"if(!confirm('Set \\u201c'+v+'\\u201d on '+fs.length+' selected image(s)?'))return;"
"gRelabelBatch(d,m,fs,function(){g_lastSp={c:m.c,l:m.l,d:v};$g('gspecbar').style.display='none';loadDay();});}"
/* Bulk "Set species" posts the whole selection to /api/relabel-batch, which
 * applies one label to many images in a SINGLE CSV rewrite. Chunked so each body
 * stays under the handler's 16 KB cap; the same progress bar drives per chunk.
 * This replaces the old one-relabel-per-image loop (one full CSV rewrite each). */
"function gRelabelBatch(d,m,fs,cb){var CH=300,i=0,tot=fs.length;gProg('Setting species',0,tot);"
"(function nxt(){if(i>=tot){setTimeout(function(){$g('gprog').style.display='none';},500);cb();return;}"
"var chunk=fs.slice(i,i+CH);"
"var body='date='+encodeURIComponent(d)+'&c='+encodeURIComponent(m.c)+'&l='+encodeURIComponent(m.l)"
"+'&files='+chunk.join(',');"
"fetch('/api/relabel-batch',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
".then(r=>r.json()).catch(()=>null).then(function(){i+=chunk.length;gProg('Setting species',i,tot);nxt();});})();}"
"function gDelSel(){var d=$g('day').value;"
"var fs=gChecks().filter(c=>c.checked).map(c=>c.dataset.f);"
"if(!fs.length){alert('No images selected');return;}"
"if(!confirm('Delete '+fs.length+' selected image'+(fs.length!==1?'s':'')+'? Cannot be undone.'))return;"
"fetch('/api/captures/delete',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&files='+fs.join(',')})"
".then(r=>r.json()).then(function(o){var n=(o&&o.deleted)||0;"
"if(n<fs.length)alert('Deleted '+n+' of '+fs.length+' \\u2013 reload and delete the rest');"
"loadDays();}).catch(()=>alert('Delete failed'));}"
"function mtLoad(){fetch('/api/days').then(r=>r.json()).then(a=>{"
"a.sort((x,y)=>y.d.localeCompare(x.d));"
"['mday','rbday'].forEach(function(id){var s=$g(id);if(!s)return;var prev=s.value;s.innerHTML='';"
"a.forEach(o=>{var op=document.createElement('option');op.value=o.d;"
"op.textContent=o.d+' ('+o.n+')';s.appendChild(op);});"
"if(prev&&[...s.options].some(o=>o.value===prev))s.value=prev;});"
"$g('mtSts').textContent=a.length?'':'no captures yet';}).catch(()=>{});}"
/* ── Backfill ROI editor: click the bird's centre on each confirmed image that
 * still lacks a motion box; the box (click centre + slider size) is written to
 * the row's roi column via POST /api/set-roi so ROI-crop training can use it. */
"var g_rb=null,g_rbc=null,g_rbbox=null;"
"function rbSts(m){$g('rbSts').textContent=m;}"
"function rbLoad(){var d=$g('rbday').value;if(!d){rbSts('pick a day');return;}rbSts('loading\\u2026');"
"fetch('/api/roi-todo?date='+encodeURIComponent(d)).then(r=>r.json()).then(function(a){"
"if(!a.length){$g('rbEdit').style.display='none';rbSts('No confirmed birds missing a box on '+d+'.');return;}"
"g_rb={d:d,list:a,i:0};$g('rbEdit').style.display='';rbSts(a.length+' bird(s) need a box.');rbShow();})"
".catch(()=>rbSts('load failed'));}"
"function rbShow(){var g=g_rb;if(!g)return;"
"if(g.i>=g.list.length){$g('rbEdit').style.display='none';rbSts('Done \\u2013 finished '+g.list.length+' image(s) for '+g.d+'. Reload to re-check.');return;}"
"var it=g.list[g.i];$g('rbImg').src='/captures/'+g.d+'/'+encodeURIComponent(it.f);"
"$g('rbProg').textContent=(g.i+1)+' / '+g.list.length+' \\u2013 '+it.sp;"
"g_rbc=[0.5,0.5];g_rbbox=null;$g('rbBox').style.display='none';}"
"function rbClick(e){var im=$g('rbImg');var r=im.getBoundingClientRect();"
"var cx=(e.clientX-r.left)/r.width,cy=(e.clientY-r.top)/r.height;"
"g_rbc=[Math.max(0,Math.min(1,cx)),Math.max(0,Math.min(1,cy))];rbDraw();}"
"function rbDraw(){if(!g_rbc)return;var im=$g('rbImg');var W=im.clientWidth,H=im.clientHeight;if(!W||!H)return;"
"var s=(+$g('rbSize').value)/100;var side=s*Math.min(W,H);"
"var l=g_rbc[0]*W-side/2,t=g_rbc[1]*H-side/2;"
"l=Math.max(0,Math.min(W-side,l));t=Math.max(0,Math.min(H-side,t));"
"var b=$g('rbBox');b.style.display='';b.style.left=l+'px';b.style.top=t+'px';b.style.width=side+'px';b.style.height=side+'px';"
"g_rbbox=[l/W,t/H,(l+side)/W,(t+side)/H];}"
"function rbSkip(){if(!g_rb)return;g_rb.i++;rbShow();}"
"function rbSave(){if(!g_rb||!g_rbbox){rbSts('click the bird first');return;}"
"var roi=g_rbbox.map(v=>Math.max(0,Math.min(1,v)).toFixed(2)).join('-');var it=g_rb.list[g_rb.i];"
"fetch('/api/set-roi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(g_rb.d)+'&f='+encodeURIComponent(it.f)+'&roi='+encodeURIComponent(roi)})"
".then(r=>r.json()).then(function(o){if(o&&o.ok){g_rb.i++;rbShow();}else rbSts((o&&o.error)||'save failed');})"
".catch(()=>rbSts('save failed'));}"
"function mtDelAll(){var d=$g('mday').value;if(!d)return;"
"if(!confirm('Delete ALL photos in '+d+'? Every image for that day is removed "
"and this cannot be undone. Statistics are separate \\u2014 use the Stats tab\\u2019s "
"Reset Statistics to clear those.'))return;"
"fetch('/api/captures/delete',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&all=1'})"
".then(r=>r.json()).then(o=>{$g('mtSts').textContent='Deleted '+(o.deleted||0)+' photo(s).';mtLoad();})"
".catch(()=>alert('Delete failed'));}"
"function mtWipeDay(){var d=$g('mday').value;if(!d)return;"
"if(!confirm('Wipe '+d+' completely? This deletes every photo AND removes that "
"day\\u2019s entries from the statistics/visit log. Cannot be undone.'))return;"
"fetch('/api/captures/delete',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'date='+encodeURIComponent(d)+'&all=1&stats=1'})"
".then(r=>r.json()).then(o=>{$g('mtSts').textContent='Removed '+(o.deleted||0)+' photo(s) and '"
"+(o.statsRemoved||0)+' log row(s).';mtLoad();}).catch(()=>alert('Wipe failed'));}"
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
"var g_statScope='day';"   /* 'day' = Today (v2.07), 'all' = all-time */
"function setStatScope(s){g_statScope=s;"
"$g('stgToday').style.opacity=s==='day'?'1':'.5';"
"$g('stgAll').style.opacity=s==='all'?'1':'.5';loadStats();}"
"function loadStats(){var q=g_statScope==='day'?'?scope=day':'';"
"Promise.all(['daily','species','hourly'].map(u=>fetch('/api/stats/'+u+q).then(r=>r.json())))"
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
"document.getElementById('sTotal').textContent=birds+' bird visit(s)'+(g_statScope==='day'?' today':(spo.since?(' since '+spo.since.replace('T',' ')):' total'))"
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
"if(!confirm('Start a new statistics window from now? The daily/species/hourly "
"charts will count only visits from this point on. Nothing is deleted \\u2014 your "
"saved photos, confirmed species and ROIs are all kept.'))return;"
"fetch('/api/stats/reset',{method:'POST'}).then(r=>r.json()).then(function(o){"
"$g('sResetSts').textContent=o.ok?('New window from '+(o.since||'now').replace('T',' ')+' \\u2713'):(o.error||'Reset failed');"
"loadStats();"
"setTimeout(function(){$g('sResetSts').textContent='';},6000);})"
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
"var g_cities=" CITIES_JS ";"
"function fillCities(){var s=$g('stLoc');if(!s||s.options.length>1)return;"
"s.innerHTML='<option value=\"\">(no location \\u2013 weaker iNat results)</option>'+"
"g_cities.map(function(c){return '<option value=\"'+c[1]+'\">'+esc(c[0])+'</option>';}).join('');}"
"function stLoad(){fetch('/api/settings').then(r=>r.json()).then(function(c){"
"fillCities();$g('stLoc').value=c.loc||'';"
"(c.mode===1?$g('stFeed'):$g('stNest')).checked=true;"
"$g('stSens').value=c.sens;stSensShow();"
"$g('stCcnt').value=c.ccnt;$g('stCivl').value=c.civl;$g('stCool').value=c.cool;"
"$g('stQtn').value=c.qtn;"
"$g('stConf').value=c.conf;$g('stCap').value=c.cap;$g('stIr').value=c.ir;"
"$g('stLang').value=c.lang;$g('stZoom').checked=c.dzoom==1;"
"$g('stFshut').checked=c.fshut==1;"
"$g('stInat').checked=c.inat==1;$g('stInatv').value=c.inatv;"
"$g('stInatCv').checked=c.inatcv==1;"
"$g('stIkey').value='';"
"$g('stIkey').placeholder=c.ikey_set?'\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep':'(not set)';"
"$g('stIkeyClr').style.display=c.ikey_set?'':'none';"
"$g('stIsess').value='';"
"$g('stIsess').placeholder=c.isess_set?'\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep':'(not set)';"
"$g('stIuser').value=c.iuser||'';"
"$g('stIpass').value='';"
"$g('stIpass').placeholder=c.ipass_set?'\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep':'(not set)';"
"$g('stCloud').value=c.cprov;"
"$g('stCkey').value='';"
"$g('stCkey').placeholder=c.ckey_set?'\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep':'(not set)';"
"$g('stCkeyClr').style.display=c.ckey_set?'':'none';"
"$g('stGkey').value='';"
"$g('stGkey').placeholder=c.gkey_set?'\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep':'(not set)';"
"$g('stGkeyClr').style.display=c.gkey_set?'':'none';"
"$g('stGmodel').value=c.gmodel||'';"
"$g('stRot').value=c.rot;$g('lvRot').value=c.rot;applyRot(c.rot);"
"$g('stRfilt').value=c.rfilt;"
"var rs=$g('stRes');"
"if(![...rs.options].some(o=>o.value==c.res)){var ro=document.createElement('option');"
"ro.value=c.res;ro.textContent='Current ('+c.res+')';rs.appendChild(ro);}"
"$g('stRes').value=c.res;$g('stContrast').value=c.contrast;$g('stAe').value=c.ae_level;g_savedRes=c.res;"
/* The box can run at something other than the saved request — a boot-time
 * degrade (camera_init steps down when a size won't initialize), or a change
 * that hasn't been rebooted into. Say which, instead of showing the request
 * and letting it read as fact (v1.96). */
"var rw=$g('stResWarn');"
"if(c.resActive!=null&&c.resActive>=0&&c.resActive!=c.res){rw.style.display='';"
"rw.textContent='\\u26a0 Running at '+c.resActiveStr+', not the size saved here. "
"Either the change above hasn\\u2019t been rebooted into yet, or that size failed to start at boot "
"(a camera fault survives a soft reboot \\u2014 a power cycle clears it). Reboot to retry; "
"your saved choice is untouched.';}"
"else rw.style.display='none';"
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
"+'&ntp='+encodeURIComponent(ntp)"
"+'&lang='+$g('stLang').value"
"+'&dzoom='+($g('stZoom').checked?1:0)"
"+'&fshut='+($g('stFshut').checked?1:0)"
"+'&inat='+($g('stInat').checked?1:0)+'&inatv='+($g('stInatv').value||60)"
"+'&inatcv='+($g('stInatCv').checked?1:0)"
"+'&loc='+encodeURIComponent($g('stLoc').value)"
"+($g('stIkey').value?'&ikey='+encodeURIComponent($g('stIkey').value):'')"
"+'&iuser='+encodeURIComponent($g('stIuser').value)"
"+($g('stIpass').value?'&ipass='+encodeURIComponent($g('stIpass').value):'')"
"+'&cprov='+$g('stCloud').value"
"+'&gmdl='+encodeURIComponent($g('stGmodel').value.trim())"
/* An empty key field means "keep the stored key" — the handler treats an
 * absent ckey/gkey as unchanged, so never send an empty one. gmdl is always
 * sent (present-only on the server): blank resets it to the default. */
"+($g('stCkey').value?'&ckey='+encodeURIComponent($g('stCkey').value):'')"
"+($g('stGkey').value?'&gkey='+encodeURIComponent($g('stGkey').value):'');"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
".then(r=>r.json()).then(function(o){"
"$g('stSts').textContent=o.ok?'Saved & applied \\u2713':'Save failed';"
"if(o.ok&&$g('stCkey').value){$g('stCkey').value='';"
"$g('stCkey').placeholder='\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep';}"
"if(o.ok&&$g('stGkey').value){$g('stGkey').value='';"
"$g('stGkey').placeholder='\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep';}"
"if(o.ok&&$g('stIkey').value){$g('stIkey').value='';"
"$g('stIkey').placeholder='\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep';}"
"$g('lvRot').value=$g('stRot').value;applyRot($g('stRot').value);"
"if(o.ok&&$g('stRes').value!==String(g_savedRes)){g_savedRes=+$g('stRes').value;"
"if(confirm('Resolution change needs a reboot to take effect. Reboot now?'))"
"fetch('/api/reboot',{method:'POST'}).then(()=>alert('Rebooting\\u2026'));}"
"setTimeout(function(){$g('stSts').textContent='';},4000);})"
".catch(function(){$g('stSts').textContent='Save failed';});}"
/* p: 1=Claude, 2=Gemini. Save any typed key first, so "paste key, press Test"
 * does what it looks like it does rather than testing the previous key. */
"function cldTest(p){var kf=p==2?'stGkey':'stCkey',sf=p==2?'stGemSts':'stCldSts',kn=p==2?'gkey':'ckey';"
"var s=$g(sf);s.textContent='\\u2026 testing';"
"var pre=$g(kf).value?fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:kn+'='+encodeURIComponent($g(kf).value)}).then(function(){stLoad();}):Promise.resolve();"
"pre.then(function(){return fetch('/api/cloud-test?p='+p);}).then(r=>r.json()).then(function(o){"
"s.textContent=(o.ok?'\\u2713 ':'\\u2717 ')+o.msg;s.style.color=o.ok?'#3c3':'#e66';})"
".catch(function(){s.textContent='\\u2717 test failed';s.style.color='#e66';});}"
/* An empty key box means "keep the stored key", so deleting one needs its own
 * action — otherwise a billable secret could never be removed from the device. */
"function ckeyClear(p){var nm=p==2?'Gemini':'Claude',cf=p==2?'gkeyclear':'ckeyclear';"
"if(!confirm('Forget the stored '+nm+' API key? If it is the active provider, "
"cloud identification stops until you paste a new one.'))return;"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:cf+'=1'})"
".then(r=>r.json()).then(function(o){$g('stSts').textContent=o.ok?'Key forgotten \\u2713':'Failed';"
"stLoad();setTimeout(function(){$g('stSts').textContent='';},4000);})"
".catch(function(){$g('stSts').textContent='Failed';});}"
/* iNaturalist: save any typed token first, then validate it against /users/me. */
"function inatTest(){var s=$g('stInatSts');s.textContent='\\u2026 testing';"
"var pre=$g('stIkey').value?fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ikey='+encodeURIComponent($g('stIkey').value)}).then(function(){stLoad();}):Promise.resolve();"
"pre.then(function(){return fetch('/api/inat-test');}).then(r=>r.json()).then(function(o){"
"s.textContent=(o.ok?'\\u2713 ':'\\u2717 ')+o.msg;s.style.color=o.ok?'#3c3':'#e66';})"
".catch(function(){s.textContent='\\u2717 test failed';s.style.color='#e66';});}"
/* Save any typed session cookie first, then mint a fresh JWT from it. */
"function inatRefresh(){var s=$g('stInatSts');s.textContent='\\u2026 refreshing token';"
"var pre=$g('stIsess').value?fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'isess='+encodeURIComponent($g('stIsess').value)}).then(function(){"
"$g('stIsess').value='';$g('stIsess').placeholder='\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep';}):Promise.resolve();"
"pre.then(function(){return fetch('/api/inat-refresh',{method:'POST'});}).then(r=>r.json()).then(function(o){"
"s.textContent=(o.ok?'\\u2713 ':'\\u2717 ')+o.msg;s.style.color=o.ok?'#3c3':'#e66';stLoad();})"
".catch(function(){s.textContent='\\u2717 refresh failed';s.style.color='#e66';});}"
/* Save any typed username/password first, then log in and mint a JWT (v2.43). */
"function inatLogin(){var s=$g('stInatSts');s.textContent='\\u2026 logging in';"
"var b=[];if($g('stIuser').value)b.push('iuser='+encodeURIComponent($g('stIuser').value));"
"if($g('stIpass').value)b.push('ipass='+encodeURIComponent($g('stIpass').value));"
"var pre=b.length?fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.join('&')}).then(function(){"
"$g('stIpass').value='';$g('stIpass').placeholder='\\u2022\\u2022\\u2022\\u2022\\u2022 saved \\u2013 leave blank to keep';}):Promise.resolve();"
"pre.then(function(){return fetch('/api/inat-login',{method:'POST'});}).then(r=>r.json()).then(function(o){"
"s.textContent=(o.ok?'\\u2713 ':'\\u2717 ')+o.msg;s.style.color=o.ok?'#3c3':'#e66';stLoad();})"
".catch(function(){s.textContent='\\u2717 login failed';s.style.color='#e66';});}"
"function ikeyClear(){if(!confirm('Forget the stored iNaturalist token? iNat primary "
"identification stops until you paste a new one.'))return;"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ikeyclear=1'})"
".then(r=>r.json()).then(function(o){$g('stSts').textContent=o.ok?'Token forgotten \\u2713':'Failed';"
"stLoad();setTimeout(function(){$g('stSts').textContent='';},4000);})"
".catch(function(){$g('stSts').textContent='Failed';});}"
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
"(d.heapInt!=null?drow('Free internal DRAM',Math.round(d.heapInt/1024)+' KB (largest block '+Math.round(d.heapIntBig/1024)+' KB)',d.heapInt<40960?'bad':(d.heapInt<81920?'':'ok'))+"
"drow('Free PSRAM',Math.round(d.heapPsram/1024)+' KB (largest block '+Math.round(d.heapPsramBig/1024)+' KB)'):'')+"
"drow('Uptime',fmtAge(d.uptime))+"
"drow('Last reset',d.resetReason,(d.resetReason==='power-on'||d.resetReason==='software')?'':'bad')+"
"drow('Device time',(d.time?d.time+' ('+d.clockSrc+')':'not set'),(d.time?(d.clockSrc==='ntp'?'ok':''):'bad'))+"
"drow('SoC temperature',(d.socTempC>-100?d.socTempC.toFixed(1)+'\\u00b0C':'n/a'),(d.socTempC>75?'bad':(d.socTempC>-100?'ok':'')))+"
"drow('WiFi reconnects',d.wifiDisc)+"
"drow('Last reconnect',fmtAge(d.wifiDiscAgo)+(d.wifiDiscAgo<0?'':' ago'))+"
"drow('HTTP sockets',(d.httpdSock==null||d.httpdSock<0?'n/a':d.httpdSock+' / '+d.httpdSockMax),"
"(d.httpdSock>=0&&d.httpdSockMax&&d.httpdSock>=d.httpdSockMax-1)?'bad':'')"
"+((d.inatCooldown>0)?drow('iNaturalist','rate-limited \\u2014 cooling down '+d.inatCooldown+'s','bad'):'');"
"$g('dWifi').innerHTML="
"drow('Network',d.apSsid||'\\u2014')+"
"drow('RSSI',d.rssi+' dBm')+drow('Channel',d.ch)+drow('Own MAC',d.mac);"
"$g('dSd').innerHTML=d.sdPresent?"
"drow('Card',d.sdCard)+drow('Size',d.sdTotalMB+' MB total, '+d.sdFreeMB+' MB free')+"
"drow('Last write',d.sdWriteOk?'OK':'FAILED',d.sdWriteOk?'ok':'bad')+"
"drow('Auto-recoveries',(d.sdRemounts||0)+(d.sdRemounts?' \\u2014 card may be wearing':''),(d.sdRemounts?'':'ok')):"
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
"var g_fwver='" FIRMWARE_VERSION "';"
/* List this repo's releases via the GitHub API (CORS-open); the device can't
 * fetch the asset itself for listing, but the browser can read the JSON. */
"function ghLoad(){var s=$g('ghRel');s.innerHTML='<option>Loading\\u2026</option>';$g('ghSts').textContent='';"
"fetch('https://api.github.com/repos/SEspe/BirdBox/releases?per_page=100')"
".then(function(r){if(!r.ok)throw 0;return r.json();}).then(function(a){s.innerHTML='';var nn=0;"
"a.forEach(function(rel){var as=(rel.assets||[]).find(function(x){return /\\.bin$/i.test(x.name);});if(!as)return;"
"var o=document.createElement('option');o.value=as.browser_download_url;"
"var cur=(rel.tag_name===('v'+g_fwver)||rel.tag_name===g_fwver);"
"o.textContent=rel.tag_name+(cur?' (running)':'')+' \\u2013 '+Math.round(as.size/1024)+' KB';"
"s.appendChild(o);nn++;});"
"if(!nn)s.innerHTML='<option>no releases with a .bin asset</option>';})"
".catch(function(){s.innerHTML='<option>couldn\\u2019t reach GitHub</option>';"
"$g('ghSts').textContent='GitHub API unreachable (device/PC offline, or rate-limited \\u2014 60 req/h unauthenticated). Try again later, or use manual upload above.';});}"
"function ghFlash(){var s=$g('ghRel');var url=s.value;"
"if(!url||url.indexOf('https://')!==0){alert('Load and pick a release first');return;}"
"var lbl=s.options[s.selectedIndex].textContent;"
"if(!confirm('Download and flash '+lbl+'?\\nThe device reboots into it; rollback protects against a bad image.'))return;"
"$g('ghProg').style.display='';$g('ghFill').style.width='0%';$g('ghTxt').textContent='connecting\\u2026';$g('ghSts').textContent='';"
"fetch('/ota/from-url',{method:'POST',headers:{'Content-Type':'text/plain'},body:url})"
".then(function(r){return r.json();}).then(function(o){if(!o.ok){$g('ghTxt').textContent=o.error||'failed to start';return;}ghPoll();})"
".catch(function(){$g('ghTxt').textContent='could not start update';});}"
"function ghPoll(){fetch('/ota/from-url').then(function(r){return r.json();}).then(function(o){"
"if(o.state==='running'){var pct=o.total>0?Math.round(o.read*100/o.total):0;"
"$g('ghFill').style.width=pct+'%';$g('ghTxt').textContent='downloading '+Math.round(o.read/1024)+' KB'+(o.total>0?(' / '+Math.round(o.total/1024)+' KB ('+pct+'%)'):'');"
"setTimeout(ghPoll,1000);}"
"else if(o.state==='done'){$g('ghFill').style.width='100%';$g('ghTxt').textContent=o.msg||'done \\u2014 rebooting';"
"$g('ghSts').textContent='Flashed \\u2014 the device is rebooting into the new version. Refresh this page in ~20 s.';}"
"else if(o.state==='failed'){$g('ghTxt').textContent='failed';"
"$g('ghSts').textContent='Update failed \\u2014 the device kept the current version. '+(o.msg||'');}"
"else setTimeout(ghPoll,1000);"
"}).catch(function(){setTimeout(ghPoll,2000);});}"
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

/* HTTP server socket pool. Raised from esp_http_server's default 7 to give
 * headroom: with a small pool, a long-lived browser UI (keep-alive polls + the
 * jerky HD stream reconnecting) can leave most slots in slow-to-reclaim states,
 * so a fresh request after idle stalls ~recv_wait_timeout seconds while an old
 * socket is purged — the "reboot cures sluggishness" root cause (FSD v2.13).
 * Needs CONFIG_LWIP_MAX_SOCKETS >= this + 3 (set to 16 in sdkconfig.defaults).
 * s_httpd is the running handle, exposed here so h_sysinfo can report the live
 * socket count (httpd_get_client_list) to confirm the pool never saturates. */
#define HTTPD_MAX_SOCKETS   12
static httpd_handle_t s_httpd = NULL;

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
/* Max visit-log rows loaded per gallery day. Every row past the cap is silently
 * dropped, and since the month's log is read front-to-back the dropped rows are
 * always the day's NEWEST — so a just-set label on a recent image reverted to
 * "unclassified" in the gallery even though it was saved and in the export
 * (v1.95; the 1200 cap was hit dead-on by a 2527-capture day). Relabel/copy-last
 * APPEND a row per no-row frame, so the row count reaches one per capture: the
 * cap must exceed a whole day's captures, not its events. Sized well past that.
 * Match is O(files×labels) but only on an occasional gallery load. */
#define GAL_MAX_LABELS  6000
/* Distinct localized species names per day — a handful in practice. Interning
 * them keeps a label at 35 bytes instead of ~104, which is what buys the cap
 * above at ~210 KB of PSRAM (too big for internal heap; largest free PSRAM
 * block is ~550 KB). GAL_SPI_NONE = no name (pool full): state still stands. */
#define GAL_MAX_SPECIES 96
#define GAL_SPI_NONE    0xFF
/* Per-frame iNat scores ("latin=pct;...") for an event's primary row (v2.29),
 * held out-of-line so the 6000-entry label table doesn't grow by the string —
 * only the ~event-count rows carry one. Longer (10-frame) strings truncate. */
#define GAL_MAX_PF      512
#define GAL_PF_LEN      176
#define GAL_PF_NONE     0xFFFF
/* base holds the capture filename ("YYYY-MM-DD_HH-MM-SS-mmm.jpg", 27 chars);
 * it was [16] — sized for the pre-v1.30 "HHMMSS.jpg" names — so every
 * millisecond-era basename truncated to 15 chars and never matched its file,
 * silently blanking all gallery species badges until v1.53. */
/* state: 0 = unclassified (no real species — sentinel "no bird"/"Unidentified"
 * row, or no row at all), 1 = classified by the model (a real species, latin
 * non-empty, not yet human-confirmed), 2 = human-confirmed (corrected set —
 * whether the user accepted the model's guess or typed their own). §3.4/v1.59 */
/* src: engine that produced the label — 0 none/legacy, 1 nordic, 2 claude,
 * 3 iNat (§3.2.3 cascade provenance). */
typedef struct { char base[32]; uint8_t spi; uint8_t pct; uint8_t state; uint8_t src;
                 uint16_t pfi; } gal_label_t;
typedef struct {
    int n, nsp, npf;
    char sp[GAL_MAX_SPECIES][72];
    char pf[GAL_MAX_PF][GAL_PF_LEN];
    gal_label_t l[GAL_MAX_LABELS];
} gal_tab_t;

/* corrected(5) non-empty is exactly states 2/4/5/6 (see gal_build_labels). */
static inline bool gal_confirmed(uint8_t state)
{
    return state == 2 || state >= 4;
}

/* Species-name pool: returns the index of `sp`, adding it if new. */
static uint8_t gal_intern(gal_tab_t *t, const char *sp)
{
    for (int i = 0; i < t->nsp; i++)
        if (strcmp(t->sp[i], sp) == 0) return (uint8_t) i;
    if (t->nsp >= GAL_MAX_SPECIES) return GAL_SPI_NONE;
    strlcpy(t->sp[t->nsp], sp, sizeof(t->sp[0]));
    return (uint8_t) t->nsp++;
}

/* One CSV field, in place; advances *p past the comma (or to the line end). */
static char *gal_next_field(char **p)
{
    char *start = *p;
    char *comma = strchr(start, ',');
    if (comma) { *comma = '\0'; *p = comma + 1; }
    else       { char *end = start + strcspn(start, "\r\n"); *end = '\0'; *p = end; }
    return start;
}

/* Fills t->l[] (up to GAL_MAX_LABELS) with basename -> localized species for
 * the given capture date, read from that month's visit log; returns the count. */
static int gal_build_labels(const char *date, gal_tab_t *t)
{
    if (!storage_sd_present()) return 0;
    char logpath[64];   /* per-day file (v2.07); helper handles the no-date case */
    storage_visit_log_path(date, logpath, sizeof(logpath));
    FILE *fp = fopen(logpath, "r");
    if (!fp) return 0;

    char match[48];
    snprintf(match, sizeof(match), "/captures/%.20s/", date);

    int count = 0;
    char line[768];   /* past the longest row incl. roi/top3/per-frame (v2.29) — a
                         split row's tail would otherwise parse as a bogus extra row */
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
        gal_next_field(&p);                        /* roi (col 8) */
        gal_next_field(&p);                        /* top3 (col 9, ';'-separated, no comma) */
        char *source    = gal_next_field(&p);      /* provenance (col 10, may be empty) */
        char *pf        = gal_next_field(&p);      /* per-frame scores (col 11, may be empty) */
        if (!species[0] || !first[0]) continue;
        char *base = strstr(first, match);         /* only this day's frames */
        if (!base) continue;
        base += strlen(match);
        if (!base[0] || strchr(base, '/')) continue;
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
        t->l[count].state = (corrected[0] && strcmp(corrected, "no bird") == 0) ? 4
                          : (corrected[0] && strcmp(corrected, "other")   == 0) ? 5
                          : (corrected[0] && strcmp(corrected, "unknown") == 0) ? 6
                          : corrected[0] ? 2
                          : latin[0]      ? 1
                          : strcmp(species, "no bird") == 0 ? 3 : 0;
        if (corrected[0]) species = corrected;   /* user label wins; latin column
                                                    holds its binomial since v1.51 */
        strlcpy(t->l[count].base, base, sizeof(t->l[count].base));
        char loc[72];
        species_localize(species, latin, g_settings.lang, loc, sizeof(loc));
        t->l[count].spi = gal_intern(t, loc);
        t->l[count].pct = (uint8_t) atoi(conf);
        t->l[count].src = strcmp(source, "nordic") == 0 ? 1
                        : strcmp(source, "claude") == 0 ? 2
                        : strcmp(source, "inat")   == 0 ? 3
                        : strcmp(source, "gemini") == 0 ? 4
                        : strcmp(source, "inatcv") == 0 ? 5 : 0;
        t->l[count].pfi = GAL_PF_NONE;
        if (pf[0] && t->npf < GAL_MAX_PF) {        /* stash the per-frame scores */
            strlcpy(t->pf[t->npf], pf, sizeof(t->pf[0]));
            t->l[count].pfi = (uint16_t) t->npf++;
        }
        count++;
    }
    if (count >= GAL_MAX_LABELS)
        ESP_LOGW(TAG, "gallery %s: label cap %d hit — newest labels dropped",
                 date, GAL_MAX_LABELS);
    fclose(fp);
    t->n = count;
    return count;
}

/* GET /api/events?date=YYYY-MM-DD — files of one capture day, each annotated
 * with its species label + confidence when it's a logged event's first frame */
#define EVENTS_OBUF 4096   /* response send-buffer; flush at this fill (v1.98) */
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

    gal_tab_t *tab = heap_caps_calloc(1, sizeof(gal_tab_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int nlabels = tab ? gal_build_labels(date, tab) : 0;

    /* Two things dominated this handler on a busy day (v1.98):
     *  - one httpd_resp_send_chunk() PER FILE — on a 2000+ image day that's 2400
     *    tiny chunked TCP writes, and the per-write overhead (not the SD, not the
     *    CPU match) was most of the ~11 s. Buffer items and flush at ~4 KB so the
     *    whole day goes out in a few dozen sends.
     *  - a per-file stat() for byte size, feeding only a cosmetic "· NN KB" tile
     *    label nobody reads — dropped (removes an SD metadata read per file).
     * A ~1-2 s floor remains: gal_build_labels reads the whole month's visit-log
     * CSV each call (unavoidable with one monthly file — a per-day log would fix
     * it, but that's a larger change touching stats/export). */
    char *obuf = malloc(EVENTS_OBUF);
    if (!obuf) {
        closedir(d); free(tab);
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    size_t used = 0;
    obuf[used++] = '[';
    struct dirent *e;
    bool first = true;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG || e->d_name[0] == '.') continue;
        const char *sp = "";
        const char *pf = "";
        int pct = 0;
        bool confirmed = false;
        int state = 0;
        int src = 0;
        bool labelled = false;
        for (int i = 0; i < nlabels; i++)
            if (strcmp(tab->l[i].base, e->d_name) == 0) {
                if (tab->l[i].spi != GAL_SPI_NONE) sp = tab->sp[tab->l[i].spi];
                if (tab->l[i].pfi != GAL_PF_NONE)  pf = tab->pf[tab->l[i].pfi];
                pct = tab->l[i].pct;
                state = tab->l[i].state;
                src = tab->l[i].src;
                confirmed = gal_confirmed(tab->l[i].state);
                labelled = true; break;
            }
        char item[480];   /* + the per-frame "pf" field (up to GAL_PF_LEN) */
        int len;
        if (labelled)
            len = snprintf(item, sizeof(item),
                           "%s{\"f\":\"%.48s\",\"sp\":\"%s\",\"pct\":%d,\"c\":%s,\"st\":%d,\"src\":%d,\"pf\":\"%s\"}",
                           first ? "" : ",", e->d_name, sp, pct,
                           confirmed ? "true" : "false", state, src, pf);
        else
            len = snprintf(item, sizeof(item), "%s{\"f\":\"%.48s\",\"st\":0}",
                           first ? "" : ",", e->d_name);
        if (used + (size_t) len > EVENTS_OBUF) {   /* flush before overflow (item < OBUF) */
            httpd_resp_send_chunk(req, obuf, used);
            used = 0;
        }
        memcpy(obuf + used, item, len);
        used += len;
        first = false;
    }
    if (used + 1 > EVENTS_OBUF) { httpd_resp_send_chunk(req, obuf, used); used = 0; }
    obuf[used++] = ']';
    httpd_resp_send_chunk(req, obuf, used);
    httpd_resp_send_chunk(req, NULL, 0);
    closedir(d);
    free(tab);
    free(obuf);
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
    /* Off-model target species (§3.2.1/§3.2.2): the operator's curated target
     * list (Artsliste.txt) — offered in the relabel picker whether or not the
     * current model can emit them, so every target species is a one-click
     * ground-truth pick. Shared with the Claude fallback + iNat mask via
     * target_species.c so the three never diverge. Any entry already emitted
     * above as a model class is skipped so it appears once (dedup keyed on the
     * Latin binomial — e.g. Lavskrike, now a real class in nordic). Norwegian
     * names live in species_i18n.c. */
    for (size_t e = 0; e < TARGET_SPECIES_N; e++) {
        /* Skip if the model already emitted this Latin binomial as a class. */
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) {
            if (!classify_label_region(i)) continue;
            const char *raw = classify_label(i);
            const char *op = strrchr(raw, '(');
            if (!op) continue;
            size_t rl = (size_t) (op - raw);
            while (rl > 0 && raw[rl-1] == ' ') rl--;
            if (rl == strlen(TARGET_SPECIES[e].latin) &&
                strncmp(raw, TARGET_SPECIES[e].latin, rl) == 0) dup = true;
        }
        if (dup) continue;
        char disp[96];
        species_localize(TARGET_SPECIES[e].common, TARGET_SPECIES[e].latin,
                         g_settings.lang, disp, sizeof(disp));
        char c_e[80], l_e[80], d_e[112];
        json_escape(c_e, sizeof(c_e), TARGET_SPECIES[e].common);
        json_escape(l_e, sizeof(l_e), TARGET_SPECIES[e].latin);
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
            char line[768];
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

/* POST /api/relabel-batch (date, c[, l], files=a.jpg,b.jpg,...) — set one species
 * on many images in a SINGLE monthly-CSV rewrite (§3.4/v1.98). The Gallery's bulk
 * "Set species" used to fire one /api/relabel per image, and since each of those
 * rewrites the whole month's CSV, a 40-image op was 40 full rewrites serialized
 * through the single httpd task — slow and UI-blocking. This does it in one pass.
 * The body is read in full (the file list spans several TCP segments), same as
 * the batch-delete handler. */
static esp_err_t h_relabel_batch(httpd_req_t *req)
{
    int cap = MIN(req->content_len, 16384);
    char *body = calloc(1, cap + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    int got = 0;
    while (got < cap) {
        int r = httpd_req_recv(req, body + got, cap - got);
        if (r <= 0) break;
        got += r;
    }
    if (got <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_OK; }
    body[got] = '\0';

    char date[16] = {0}, common[80] = {0}, latin[80] = {0};
    httpd_query_key_value(body, "date", date, sizeof(date));
    httpd_query_key_value(body, "c",    common, sizeof(common));
    httpd_query_key_value(body, "l",    latin,  sizeof(latin));
    url_decode(date); url_decode(common); url_decode(latin);

    httpd_resp_set_type(req, "application/json");
    if (strlen(date) != 10 || !common[0]) {
        free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad params"); return ESP_OK;
    }

    /* files=a.jpg,b.jpg,... parsed straight from the body (not a fixed field
     * buffer) so a long selection isn't truncated. Filenames are safe chars, so
     * no url-decode; reject any that could escape the day dir. Pointers index
     * into body, valid until it's freed below — after storage_relabel_batch. */
    char *fl = strstr(body, "files=");
    if (!fl) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no files"); return ESP_OK; }
    fl += 6;
    char *amp = strchr(fl, '&');
    if (amp) *amp = '\0';
    int nmax = 1;
    for (char *q = fl; *q; q++) if (*q == ',') nmax++;
    const char **names = malloc(nmax * sizeof(char *));
    if (!names) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    int n = 0;
    for (char *tok = fl; tok && *tok; ) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';
        if (tok[0] && !strchr(tok, '/') && !strstr(tok, "..") && !strchr(tok, '\\'))
            names[n++] = tok;
        if (!comma) break;
        tok = comma + 1;
    }
    if (n == 0) { free(names); free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no valid files"); return ESP_OK; }

    int applied = 0;
    esp_err_t err = storage_relabel_batch(date, names, n, common, latin, &applied);
    free(names);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"relabel failed\"}");
        return ESP_OK;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"applied\":%d,\"requested\":%d}", applied, n);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /api/roi-todo?date=YYYY-MM-DD — confirmed real-bird rows of one day that
 * still lack a motion ROI (§3.4/v1.99). The backfill worklist: images the human
 * confirmed to a species before always-on ROI logging, so they have no bird box
 * for ROI-crop training. Streams [{"f":"NAME.jpg","sp":"Dompap"}, ...]; the Maint
 * ROI editor walks it, and a click writes the box via POST /api/set-roi. Sentinel
 * corrections (no bird / other / unknown) are excluded — those aren't birds. */
static esp_err_t h_roi_todo(httpd_req_t *req)
{
    char query[64] = {0}, date[16] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "date", date, sizeof(date));
    httpd_resp_set_type(req, "application/json");
    if (strlen(date) != 10) { httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    for (const char *c = date; *c; c++)
        if (!isalnum((unsigned char) *c) && *c != '-') { httpd_resp_sendstr(req, "[]"); return ESP_OK; }

    char logpath[64], match[48];
    storage_visit_log_path(date, logpath, sizeof(logpath));
    snprintf(match, sizeof(match), "/captures/%.10s/", date);
    FILE *fp = storage_sd_present() ? fopen(logpath, "r") : NULL;
    if (!fp) { httpd_resp_sendstr(req, "[]"); return ESP_OK; }

    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    char line[768];
    bool header = true;
    while (fgets(line, sizeof(line), fp)) {
        if (header) { header = false; continue; }
        if (line[0] == '\0' || line[0] == '\n') continue;
        char *p = line;
        gal_next_field(&p);                        /* timestamp */
        gal_next_field(&p);                        /* species (model) */
        gal_next_field(&p);                        /* confidence */
        gal_next_field(&p);                        /* frames */
        char *first_fr  = gal_next_field(&p);
        char *corrected = gal_next_field(&p);
        char *latin     = gal_next_field(&p);      /* binomial, for localization */
        char *roi       = gal_next_field(&p);
        if (!corrected[0] || roi[0]) continue;     /* confirmed rows still missing a roi */
        if (!strcmp(corrected, "no bird") || !strcmp(corrected, "other") ||
            !strcmp(corrected, "unknown")) continue;                 /* birds only */
        char *base = strstr(first_fr, match);
        if (!base) continue;
        base += strlen(match);
        if (!base[0] || strchr(base, '/')) continue;
        char disp[96];                             /* localized name (+ Latin) per settings.lang */
        species_localize(corrected, latin, g_settings.lang, disp, sizeof(disp));
        char f_e[64], s_e[160];
        json_escape(f_e, sizeof(f_e), base);
        json_escape(s_e, sizeof(s_e), disp);
        char item[280];
        int len = snprintf(item, sizeof(item), "%s{\"f\":\"%s\",\"sp\":\"%s\"}",
                           first ? "" : ",", f_e, s_e);
        httpd_resp_send_chunk(req, item, len);
        first = false;
    }
    fclose(fp);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/set-roi (date, f, roi) — backfill one row's motion ROI (§3.4/v1.99).
 * `roi` is "x0-y0-x1-y1", each a fraction in [0,1] with x0<x1, y0<y1: the box the
 * Maint editor built from the click center + size. Written verbatim to the roi
 * column so ROI-crop training (and detect_zoom inference) can use it. */
static esp_err_t h_set_roi(httpd_req_t *req)
{
    char body[128] = {0};
    int rlen = MIN(req->content_len, (int) sizeof(body) - 1);
    int got = 0;
    while (got < rlen) {
        int r = httpd_req_recv(req, body + got, rlen - got);
        if (r <= 0) break;
        got += r;
    }
    char date[16] = {0}, file[80] = {0}, roi[32] = {0};
    httpd_query_key_value(body, "date", date, sizeof(date));
    httpd_query_key_value(body, "f",    file, sizeof(file));
    httpd_query_key_value(body, "roi",  roi,  sizeof(roi));
    url_decode(date); url_decode(file); url_decode(roi);

    httpd_resp_set_type(req, "application/json");
    float x0, y0, x1, y1;
    bool shape_ok = sscanf(roi, "%f-%f-%f-%f", &x0, &y0, &x1, &y1) == 4 &&
                    x0 >= 0.f && y0 >= 0.f && x1 <= 1.f && y1 <= 1.f &&
                    x1 > x0 && y1 > y0;
    if (strlen(date) != 10 || !file[0] || !shape_ok ||
        strstr(file, "..") || strchr(file, '/') || strchr(file, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad params");
        return ESP_OK;
    }
    esp_err_t err = storage_set_roi(date, file, roi);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"no row for that image\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"set roi failed\"}");
        return ESP_OK;
    }
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
    /* Read the whole body, not just the first TCP segment: a large multi-
     * select list (files=a.jpg,b.jpg,...) easily spans several packets, and a
     * single httpd_req_recv() would truncate it — silently deleting only the
     * files that fit in the first ~1.4 KB. Loop like every other batch handler. */
    int got = 0;
    while (got < cap) {
        int r = httpd_req_recv(req, body + got, cap - got);
        if (r <= 0) break;
        got += r;
    }
    if (got <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_OK; }
    body[got] = '\0';

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
/* Resolve the Stats scope from ?scope=day → today's "YYYY-MM-DD" (v2.07 Today
 * view); anything else → "" (all-time). Guards the pre-SNTP ~1970 clock: if the
 * clock isn't synced, returns "" (all-time) rather than a bogus 1970 day. The
 * empty string is what stats_collect_scoped treats as all-time. */
static const char *stats_scope_date(httpd_req_t *req, char *buf, size_t sz)
{
    buf[0] = '\0';
    char q[48] = {0}, scope[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "scope", scope, sizeof(scope));
    if (strcmp(scope, "day") == 0) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        if (tmv.tm_year + 1900 >= 2020)
            strftime(buf, sz, "%Y-%m-%d", &tmv);
    }
    return buf;
}

static esp_err_t h_stats_daily(httpd_req_t *req)
{
    stats_t *st = calloc(1, sizeof(stats_t));
    if (!st) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); return ESP_OK; }
    char scope[12];
    stats_collect_scoped(st, stats_scope_date(req, scope, sizeof(scope)));
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
    char scope[12];
    stats_collect_scoped(st, stats_scope_date(req, scope, sizeof(scope)));
    httpd_resp_set_type(req, "application/json");
    /* Object, not a bare array (v1.46): confirmed false positives ("no bird"
     * rows, §3.4) ride alongside the species rows instead of polluting them. */
    char head[96], since_e[28];
    json_escape(since_e, sizeof(since_e), g_settings.stats_reset_ts);
    int hl = snprintf(head, sizeof(head), "{\"falsePos\":%lu,\"since\":\"%s\",\"rows\":[",
                      (unsigned long) st->false_pos, since_e);
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
    char scope[12];
    stats_collect_scoped(st, stats_scope_date(req, scope, sizeof(scope)));
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

/* POST /api/stats/reset — start a fresh statistics window (FSD §3.4). NON-
 * DESTRUCTIVE (v2.02): records `now` as the stats-reset epoch, so stats count
 * only visits at/after it. The visit-log CSVs are NOT deleted, so every
 * confirmed label + ROI (the gallery and the retrain export) survives a reset —
 * statistics are decoupled from the ground-truth data. Requires an NTP-synced
 * clock; otherwise `now` is ~1970 and the epoch would filter nothing. */
static esp_err_t h_stats_reset(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2020) {
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"clock not synced yet \\u2014 wait for NTP time, then reset\"}");
        return ESP_OK;
    }
    strftime(g_settings.stats_reset_ts, sizeof(g_settings.stats_reset_ts),
             "%Y-%m-%dT%H:%M:%S", &tm);
    settings_save();
    ESP_LOGI(TAG, "stats reset epoch set to %s (log preserved)", g_settings.stats_reset_ts);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"since\":\"%s\"}", g_settings.stats_reset_ts);
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

/* Whether `key` (e.g. "region=") appears as a real field in the body — i.e.
 * form_field would read it — distinguishing an ABSENT field from one that is
 * present but empty. Needed for fields where "" is a legal value (region ""
 * = auto-pick), so a partial POST that omits the field keeps the current value
 * instead of blanking it. */
static bool has_field(const char *body, const char *key)
{
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if (p == body || p[-1] == '&') return true;
        p++;
    }
    return false;
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
    char buf[880];   /* truncation here would emit malformed JSON and take the
                        whole Settings tab down — keep headroom (v1.96 added
                        resActive/resActiveStr, ~45 B; inat fields ~28 B) */
    /* ckey_set, never the key itself: this reply is world-readable on the LAN,
     * and the same posture as /api/wificfg (which reports configured SSIDs but
     * never the passwords). */
    int n = snprintf(buf, sizeof(buf),
        "{\"mode\":%d,\"sens\":%u,\"ccnt\":%u,\"civl\":%u,\"cool\":%u,"
        "\"conf\":%u,\"cap\":%u,\"qual\":%u,\"ir\":%u,\"rot\":%u,\"rfilt\":%u,"
        "\"res\":%u,\"resActive\":%d,\"resActiveStr\":\"%s\","
        "\"contrast\":%d,\"ae_level\":%d,\"tz\":\"%s\","
        "\"region\":\"%s\",\"ntp\":\"%s\",\"lang\":%u,"
        "\"zone\":\"%s\",\"dzoom\":%u,\"fshut\":%u,\"tta\":%u,\"qtn\":%u,"
        "\"inat\":%u,\"inatv\":%u,"
        "\"cprov\":%u,\"ckey_set\":%s,\"gkey_set\":%s,\"gmodel\":\"%s\","
        "\"ondev\":%u,\"inatcv\":%u,\"ikey_set\":%s,\"isess_set\":%s,"
        "\"iuser\":\"%s\",\"ipass_set\":%s,\"loc\":\"%s\",\"models\":[",
        g_settings.mode, g_settings.motion_sensitivity, g_settings.capture_count,
        g_settings.capture_interval_ms, g_settings.cooldown_s,
        g_settings.confidence_pct, g_settings.sd_cap_pct,
        g_settings.stream_quality, g_settings.ir_led_mode, (unsigned) g_settings.rotation,
        (unsigned) g_settings.region_filter,
        /* res = the standing request (what's in NVS); resActive = what the
         * camera actually came up at. They differ after a degraded boot, or
         * after a res change that hasn't been rebooted into yet — the Settings
         * tab says so rather than letting the request quietly read as fact. */
        (unsigned) g_settings.resolution,
        camera_active_res() == CAMERA_RES_NONE ? -1 : (int) camera_active_res(),
        camera_framesize_str(),
        (int) g_settings.contrast,
        (int) g_settings.ae_level,
        g_settings.timezone, g_settings.region, g_settings.ntp_server,
        (unsigned) g_settings.lang, zone, (unsigned) g_settings.detect_zoom,
        (unsigned) g_settings.fast_shutter, (unsigned) g_settings.tta,
        (unsigned) g_settings.detect_quarantine_s,
        (unsigned) g_settings.inat_periodic_enabled,
        (unsigned) g_settings.inat_periodic_interval_min,
        (unsigned) g_settings.cloud_provider,
        g_settings.claude_key[0] ? "true" : "false",
        g_settings.gemini_key[0] ? "true" : "false",
        g_settings.gemini_model,   /* [a-z0-9.-] only (validated on save) — JSON-safe */
        (unsigned) g_settings.ondevice_enabled,
        (unsigned) g_settings.inat_cv_enabled,
        g_settings.inat_key[0] ? "true" : "false",
        g_settings.inat_session[0] ? "true" : "false",
        g_settings.inat_user,   /* quote/backslash/control chars rejected on save — JSON-safe */
        g_settings.inat_pass[0] ? "true" : "false",
        g_settings.inat_loc);   /* [0-9.,-] only (validated on save) — JSON-safe */
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

/* Make a pasted iNaturalist token robust (v2.35). The operator copies it from
 * inaturalist.org/users/api_token, which serves {"api_token":"eyJ..."} — so
 * accept the whole JSON (pull the value out) as well as the bare token, and
 * strip stray whitespace/quotes a copy-paste drags along. Without this a wrapped
 * or padded token is silently rejected (the '"' guard) and looks like it "won't
 * take". Operates in place; a bare JWT (base64url, no quotes) passes untouched. */
static void sanitize_inat_token(char *t)
{
    char *j = strstr(t, "\"api_token\"");     /* the quoted JSON key only */
    if (j) {
        j += strlen("\"api_token\"");
        while (*j && *j != ':') j++;
        if (*j == ':') j++;
        while (*j == ' ' || *j == '\t' || *j == '"') j++;
        memmove(t, j, strlen(j) + 1);
    }
    size_t n = strlen(t);                      /* trim trailing junk */
    while (n && (t[n-1] == '\n' || t[n-1] == '\r' || t[n-1] == ' ' ||
                 t[n-1] == '\t' || t[n-1] == '"'  || t[n-1] == '}' || t[n-1] == ','))
        t[--n] = '\0';
    char *s = t;                               /* trim leading junk */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '"') s++;
    if (s != t) memmove(t, s, strlen(s) + 1);
}

/* Normalise a pasted _inaturalist_session cookie (v2.36): accept either the bare
 * value or a "_inaturalist_session=…" (or a full "a=b; _inaturalist_session=…")
 * copy, keep just the value, and drop any trailing "; other=…" and whitespace so
 * it is safe in a Cookie header. In place. */
static void sanitize_inat_cookie(char *c)
{
    char *k = strstr(c, "_inaturalist_session=");
    if (k) { k += strlen("_inaturalist_session="); memmove(c, k, strlen(k) + 1); }
    char *semi = strchr(c, ';');
    if (semi) *semi = '\0';
    size_t n = strlen(c);
    while (n && (c[n-1] == '\n' || c[n-1] == '\r' || c[n-1] == ' ' || c[n-1] == '\t'))
        c[--n] = '\0';
    char *s = c;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s != c) memmove(c, s, strlen(s) + 1);
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    /* Sized with headroom: a body that overruns this is silently truncated,
     * which drops whichever fields land past the cut with no error anywhere.
     * A full save is ~330 B (~460 with the zone mask and a Claude key ~108
     * chars); an iNaturalist JWT (ikey) is ~300-800 chars and lands LAST, so the
     * buffer must clear it comfortably. Read with a LOOP — a >1 KB body spans
     * TCP segments and a single recv would grab only the first (the v1.89
     * empty-fields/partial-read class of bug). */
    char body[2048] = {0};
    int  cap = MIN(req->content_len, (int) sizeof(body) - 1), len = 0;
    while (len < cap) {
        int r = httpd_req_recv(req, body + len, cap - len);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        len += r;
    }
    body[len] = '\0';

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
    /* region names the active model file (§3.2); "" = auto-pick. Apply ONLY
     * when the field is actually present, so a partial POST (any tab's save
     * that doesn't carry region=) can't blank the model selection — that
     * silently booted the wrong model after the next reboot. Empty-but-present
     * is still honoured (the Settings dropdown's "auto" choice). Reject path
     * escapes / JSON-breakers. */
    if (has_field(body, "region=") &&
        !strstr(region, "..") && !strchr(region, '/') &&
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

    /* Cloud classifier (§3.2.3). cprov selects the active provider (0 off, 1
     * Claude, 2 Gemini) — one at a time. An absent/empty ckey/gkey keeps that
     * stored key: the UI sends a key field only when the user actually typed a
     * new one, so saving any other setting (or restoring a backup, which carries
     * no key) can't wipe it. Deleting a key is an explicit act: ckeyclear /
     * gkeyclear, the Settings tab's "Forget stored key" buttons. Forgetting the
     * key of the currently-selected provider turns the selector off, since it
     * can no longer run. */
    g_settings.cloud_provider = field_num(body, "cprov=", 0, 2, g_settings.cloud_provider);
    g_settings.inat_periodic_enabled = field_num(body, "inat=", 0, 1, g_settings.inat_periodic_enabled);
    g_settings.inat_periodic_interval_min =
        field_num(body, "inatv=", 5, 1440, g_settings.inat_periodic_interval_min);
    if (field_num(body, "ckeyclear=", 0, 1, 0) == 1) {
        g_settings.claude_key[0] = '\0';
        if (g_settings.cloud_provider == CLOUD_CLAUDE) g_settings.cloud_provider = CLOUD_OFF;
    } else {
        char ckey[160];   /* must exceed settings.claude_key so a real
                           * ~108-char sk-ant- key is never clipped here */
        form_field(body, "ckey=", ckey, sizeof(ckey));
        if (ckey[0] && !strchr(ckey, '"') && !strchr(ckey, '\\'))
            strlcpy(g_settings.claude_key, ckey, sizeof(g_settings.claude_key));
    }
    if (field_num(body, "gkeyclear=", 0, 1, 0) == 1) {
        g_settings.gemini_key[0] = '\0';
        if (g_settings.cloud_provider == CLOUD_GEMINI) g_settings.cloud_provider = CLOUD_OFF;
    } else {
        char gkey[160];
        form_field(body, "gkey=", gkey, sizeof(gkey));
        if (gkey[0] && !strchr(gkey, '"') && !strchr(gkey, '\\'))
            strlcpy(g_settings.gemini_key, gkey, sizeof(g_settings.gemini_key));
    }
    /* Gemini model id. Present-only (like the keys): a POST without gmdl leaves
     * it unchanged. An explicit empty value ("gmdl=") resets to the gemini.c
     * default. Validated to [a-z0-9.-] so it's safe both in the request URL and
     * echoed unescaped into the /api/settings JSON. */
    if (has_field(body, "gmdl=")) {
        char gmdl[64];
        form_field(body, "gmdl=", gmdl, sizeof(gmdl));
        bool ok = true;
        for (char *p = gmdl; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                  *p == '-' || *p == '.')) { ok = false; break; }
        if (ok) strlcpy(g_settings.gemini_model, gmdl, sizeof(g_settings.gemini_model));
    }

    /* iNaturalist online CV — the PRIMARY tier (§3.2.3). Same present-only token
     * handling as the cloud keys: an absent ikey keeps the stored JWT; an
     * explicit ikeyclear=1 forgets it (and turns the tier off). The JWT is long
     * (~300-800 chars), so extract it into a heap buffer, not the httpd stack. */
    g_settings.ondevice_enabled = field_num(body, "ondev=", 0, 1, g_settings.ondevice_enabled);
    g_settings.inat_cv_enabled = field_num(body, "inatcv=", 0, 1, g_settings.inat_cv_enabled);
    if (field_num(body, "ikeyclear=", 0, 1, 0) == 1) {
        g_settings.inat_key[0] = '\0';
        g_settings.inat_cv_enabled = 0;
    } else {
        char *ikey = malloc(sizeof(g_settings.inat_key) + 64);
        if (ikey) {
            form_field(body, "ikey=", ikey, sizeof(g_settings.inat_key) + 64);
            sanitize_inat_token(ikey);   /* accept the {"api_token":".."} JSON + strip paste junk */
            if (ikey[0] && !strchr(ikey, '"') && !strchr(ikey, '\\')) {
                strlcpy(g_settings.inat_key, ikey, sizeof(g_settings.inat_key));
                inat_token_changed();    /* new token → clear stale cooldown, take effect now */
            }
            free(ikey);
        }
    }
    /* iNaturalist session cookie (v2.36): present-only, like the keys. Used to
     * auto-refresh the JWT (POST /api/inat-refresh). isessclear=1 forgets it. */
    if (field_num(body, "isessclear=", 0, 1, 0) == 1) {
        g_settings.inat_session[0] = '\0';
    } else if (has_field(body, "isess=")) {
        char *isess = malloc(sizeof(g_settings.inat_session) + 64);
        if (isess) {
            form_field(body, "isess=", isess, sizeof(g_settings.inat_session) + 64);
            sanitize_inat_cookie(isess);
            if (isess[0] && !strchr(isess, '\r') && !strchr(isess, '\n'))
                strlcpy(g_settings.inat_session, isess, sizeof(g_settings.inat_session));
            free(isess);
        }
    }
    /* iNaturalist account credentials for the self-service login (v2.43). The
     * username is echoed back in the settings GET, so reject anything that would
     * break the JSON or an HTTP header; the password is write-only (never echoed,
     * only an ipass_set flag). iuserclear=1 forgets both. */
    if (field_num(body, "iuserclear=", 0, 1, 0) == 1) {
        g_settings.inat_user[0] = '\0';
        g_settings.inat_pass[0] = '\0';
    } else {
        if (has_field(body, "iuser=")) {
            char u[64];
            form_field(body, "iuser=", u, sizeof(u));
            if (!strchr(u, '"') && !strchr(u, '\\') && !strchr(u, '\r') && !strchr(u, '\n'))
                strlcpy(g_settings.inat_user, u, sizeof(g_settings.inat_user));
        }
        if (has_field(body, "ipass=")) {
            char p[64];
            form_field(body, "ipass=", p, sizeof(p));
            if (p[0] && !strchr(p, '\r') && !strchr(p, '\n'))
                strlcpy(g_settings.inat_pass, p, sizeof(g_settings.inat_pass));
        }
    }
    /* iNat geo hint "lat,lng" from the capital dropdown. Present-only. Validated
     * to [0-9.,-] so it is safe in the multipart request and the JSON echo. */
    if (has_field(body, "loc=")) {
        char loc[24];
        form_field(body, "loc=", loc, sizeof(loc));
        bool ok = true;
        for (char *p = loc; *p; p++)
            if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ',' || *p == '-')) { ok = false; break; }
        if (ok) strlcpy(g_settings.inat_loc, loc, sizeof(g_settings.inat_loc));
    }

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
    /* The API keys are deliberately NOT exported: they are billable secrets and
     * this file gets mailed around and dropped into cloud folders. Omitting
     * ckey/gkey reads as "keep current" on restore (see h_settings_post), so the
     * only cost is re-pasting a key after an NVS wipe. */
    char buf[700];
    int n = snprintf(buf, sizeof(buf),
        "mode=%s&sens=%u&ccnt=%u&civl=%u&cool=%u&conf=%u&cap=%u&qual=%u&ir=%u"
        "&rot=%u&rfilt=%u&res=%u&contrast=%d&ael=%d&tz=%s&region=%s&ntp=%s"
        "&lang=%u&zone=%s&dzoom=%u&fshut=%u&tta=%u&qtn=%u&inat=%u&inatv=%u&cprov=%u&gmdl=%s"
        "&ondev=%u&inatcv=%u&loc=%s",
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
        (unsigned) g_settings.detect_quarantine_s,
        (unsigned) g_settings.inat_periodic_enabled,
        (unsigned) g_settings.inat_periodic_interval_min,
        (unsigned) g_settings.cloud_provider,
        g_settings.gemini_model,
        (unsigned) g_settings.ondevice_enabled,
        (unsigned) g_settings.inat_cv_enabled,
        g_settings.inat_loc);
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

    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"version\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"heap\":%lu,\"uptime\":%lld,\"portal\":%s,\"wifiReconnects\":%lu,"
        "\"sdPresent\":%s,\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,\"sdWriteOk\":%s,"
        "\"time\":\"%s\",\"clockSrc\":\"%s\","
        "\"motion\":%s,\"detect\":%s,\"quarantineS\":%u,"
        "\"streamUsed\":%d,\"streamMax\":%d,"
        "\"events\":%lu,\"lastEvent\":\"%s\",\"species\":\"%s\",\"spConf\":%u,"
        "\"spLive\":%s,\"evStart\":%lu,\"clsSeq\":%lu,\"spFile\":\"%s\","
        "\"clsBusy\":%s}",
        FIRMWARE_NAME, FIRMWARE_VERSION, ip, rssi, ch,
        (unsigned long) esp_get_free_heap_size(),
        esp_timer_get_time() / 1000000,
        wifi_in_portal_mode() ? "true" : "false",
        (unsigned long) g_wifi_disconnect_count,
        storage_sd_present() ? "true" : "false",
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        storage_last_write_ok() ? "true" : "false",
        tstr, tsrc,
        motion_active() ? "true" : "false",
        motion_detection_enabled() ? "true" : "false",
        (unsigned) motion_quarantine_remaining_s(),
        s_stream_clients, STREAM_MAX_CLIENTS,
        (unsigned long) capture_event_count(),
        capture_last_event_path(),
        classify_last_species()[0] ? species : "",
        (unsigned) classify_last_confidence(),
        classify_last_event_identified() ? "true" : "false",
        (unsigned long) motion_trigger_count(),
        (unsigned long) classify_result_seq(),
        classify_last_file(),   /* "/captures/DATE/FILE.jpg" — device-generated, JSON-safe */
        classify_busy() ? "true" : "false");
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

    /* Live httpd socket usage — the diagnostic for the "reboot cures sluggishness"
     * root cause (FSD v2.13). If httpdSock climbs toward httpdSockMax over hours,
     * the pool is congesting; healthy is a handful. */
    int httpd_sock = -1;
    if (s_httpd) {
        int fds[HTTPD_MAX_SOCKETS];
        size_t nfds = HTTPD_MAX_SOCKETS;
        if (httpd_get_client_list(s_httpd, &nfds, fds) == ESP_OK) httpd_sock = (int) nfds;
    }

    char buf[1152];
    int n = snprintf(buf, sizeof(buf),
        "{\"heap\":%lu,\"heapMin\":%lu,\"heapMinAgo\":%lld,"
        "\"heapInt\":%lu,\"heapIntBig\":%lu,\"heapPsram\":%lu,\"heapPsramBig\":%lu,"
        "\"uptime\":%lld,"
        "\"resetReason\":\"%s\","
        "\"wifiDisc\":%lu,\"wifiDiscAgo\":%lld,\"mac\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"apSsid\":\"%s\",\"time\":\"%s\",\"clockSrc\":\"%s\","
        "\"sdPresent\":%s,\"sdCard\":\"%s\",\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,"
        "\"sdWriteOk\":%s,\"sdRemounts\":%lu,"
        "\"camPresent\":%s,\"camPid\":%d,\"camRes\":\"%s\",\"camQuality\":%u,"
        "\"camRecoveries\":%lu,\"camRecoveryAgo\":%d,\"camFault\":%s,"
        "\"socTempC\":%.1f,\"motionTriggers\":%lu,"
        "\"lastInferenceMs\":%ld,\"clsModel\":\"%s\",\"clsLabels\":%d,\"clsRegion\":%d,"
        "\"httpdSock\":%d,\"httpdSockMax\":%d,\"inatCooldown\":%d}",
        (unsigned long) esp_get_free_heap_size(),
        (unsigned long) g_heap_min,
        (long long) ((now_us - g_heap_min_ts_us) / 1000000),
        (unsigned long) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned long) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        now_us / 1000000,
        reset_reason_str(),
        (unsigned long) g_wifi_disconnect_count,
        g_wifi_last_disc_ts_us ? (long long) ((now_us - g_wifi_last_disc_ts_us) / 1000000) : -1,
        mac_str, rssi, ch, apssid, tstr, tsrc,
        storage_sd_present() ? "true" : "false", card,
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        storage_last_write_ok() ? "true" : "false",
        (unsigned long) storage_remount_count(),
        camera_available() ? "true" : "false", camera_get_pid(),
        camera_framesize_str(), g_settings.stream_quality,
        (unsigned long) camera_recovery_count(), camera_last_recovery_ago_s(),
        camera_fault() ? "true" : "false",
        soc_temp_c(), (unsigned long) motion_trigger_count(),
        (long) classify_last_duration_ms(),
        classify_model_name(), classify_label_count(), classify_region_matches(),
        httpd_sock, HTTPD_MAX_SOCKETS, inat_cooldown_s());
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

/* ── OTA from a GitHub release URL (device-side HTTPS download) ──────────────
 * The browser can LIST releases (the GitHub API sends CORS *) but cannot
 * download the .bin (release-assets.githubusercontent.com sends no CORS
 * header), so the device fetches + flashes it itself over TLS. Repo-locked:
 * the URL must start with OTAU_URL_PREFIX, so this can only ever flash this
 * project's own signed releases, never an arbitrary binary. Runs in its own
 * task (TLS handshake + ~1.3 MB download takes many seconds); progress is
 * polled via GET. esp_https_ota does begin/write/verify/set-boot and inherits
 * the same rollback safety as /ota/upload. TLS buffers live in internal DRAM
 * (measured ~180 KB free, steady under load); dynamic buffers keep the peak
 * small (sdkconfig.defaults). */
#define OTAU_URL_PREFIX "https://github.com/SEspe/BirdBox/releases/download/"
typedef enum { OTAU_IDLE = 0, OTAU_RUNNING, OTAU_DONE, OTAU_FAILED } otau_state_t;
static volatile otau_state_t s_otau_state = OTAU_IDLE;
static volatile int s_otau_read = 0, s_otau_total = 0;
static char s_otau_msg[96] = "";
static char s_otau_url[300] = "";

static void otau_fail(const char *m)
{
    snprintf(s_otau_msg, sizeof(s_otau_msg), "%s", m);
    s_otau_state = OTAU_FAILED;
    ESP_LOGE(TAG, "OTA-from-URL failed: %s", m);
}

static void otau_task(void *arg)
{
    esp_http_client_config_t http_cfg = {
        .url               = s_otau_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        .buffer_size       = 2048,
        .buffer_size_tx    = 4096,   /* the signed redirect URL is ~900 chars */
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };
    esp_https_ota_handle_t h = NULL;

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK || h == NULL) { otau_fail("could not connect to GitHub"); vTaskDelete(NULL); return; }

    s_otau_total = esp_https_ota_get_image_size(h);
    do {
        err = esp_https_ota_perform(h);
        s_otau_read = esp_https_ota_get_image_len_read(h);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            snprintf(s_otau_msg, sizeof(s_otau_msg), "downloaded %d KB — rebooting", s_otau_read / 1024);
            s_otau_state = OTAU_DONE;
            ESP_LOGW(TAG, "OTA-from-URL complete — rebooting into new image");
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        } else {
            otau_fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation" : "flash finalize failed");
        }
    } else {
        esp_https_ota_abort(h);
        otau_fail("download interrupted");
    }
    vTaskDelete(NULL);
}

static bool otau_url_allowed(const char *u)
{
    return strncmp(u, OTAU_URL_PREFIX, strlen(OTAU_URL_PREFIX)) == 0 &&
           !strstr(u, "..") && !strchr(u, ' ') && strlen(u) < sizeof(s_otau_url);
}

/* POST /ota/from-url — body is the release asset URL (repo-locked). Starts the
 * background download+flash; poll GET /ota/from-url for progress. */
static esp_err_t h_ota_from_url(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (s_otau_state == OTAU_RUNNING) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"an update is already running\"}");
        return ESP_OK;
    }
    char url[300] = {0};
    int rlen = MIN(req->content_len, (int) sizeof(url) - 1);
    int got = 0;
    while (got < rlen) {
        int r = httpd_req_recv(req, url + got, rlen - got);
        if (r <= 0) break;
        got += r;
    }
    char *u = url;
    if (strncmp(u, "url=", 4) == 0) { u += 4; url_decode(u); }   /* accept raw url or url=... */
    for (int i = (int) strlen(u) - 1; i >= 0 && (u[i] == '\n' || u[i] == '\r' || u[i] == ' '); i--)
        u[i] = 0;

    if (!otau_url_allowed(u)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"url must be a SEspe/BirdBox release asset\"}");
        return ESP_OK;
    }
    snprintf(s_otau_url, sizeof(s_otau_url), "%s", u);
    s_otau_read = 0; s_otau_total = 0; s_otau_msg[0] = 0;
    s_otau_state = OTAU_RUNNING;
    if (xTaskCreate(otau_task, "otau", 10240, NULL, 5, NULL) != pdPASS) {
        s_otau_state = OTAU_IDLE;
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory for update task\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA-from-URL started: %s", s_otau_url);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /ota/from-url — progress of the running/last download. */
static esp_err_t h_ota_from_url_status(httpd_req_t *req)
{
    const char *st = s_otau_state == OTAU_RUNNING ? "running" :
                     s_otau_state == OTAU_DONE    ? "done" :
                     s_otau_state == OTAU_FAILED  ? "failed" : "idle";
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"read\":%d,\"total\":%d,\"msg\":\"%s\"}",
        st, s_otau_read, s_otau_total, s_otau_msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
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

    /* latin + common (the English canonical) are emitted too so the Gallery can
     * seed the 📋 copy-last fast-path from a confident cloud/model identify —
     * they're what /api/relabel needs (l/c), which the localized `species`
     * display name can't supply. Both are already CSV/JSON-scrubbed at the
     * classifier boundary. Empty latin ⇒ "no bird"/"Unidentified", so the client
     * only seeds copy-last when latin is present. */
    char buf[912];
    snprintf(buf, sizeof(buf),
        "{\"species\":\"%s\",\"latin\":\"%s\",\"common\":\"%s\","
        "\"confidence\":%u,\"durationMs\":%ld,\"saved\":%s,\"top3\":["
        "{\"label\":\"%s\",\"pct\":%u},{\"label\":\"%s\",\"pct\":%u},"
        "{\"label\":\"%s\",\"pct\":%u}]}",
        species, r->latin, r->species,
        r->confidence_pct, (long) r->duration_ms, saved ? "true" : "false",
        l0, r->top_pct[0], l1, r->top_pct[1], l2, r->top_pct[2]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Species-ID requests are offloaded to a worker task (see ident_dispatch, just
 * below the shared identify helpers) so a slow on-device inference or Claude
 * round-trip can never freeze the single httpd task (FSD v1.97). */
typedef enum {
    IDENT_MODEL_POST,    /* POST /api/classify      — inference on a posted JPEG */
    IDENT_MODEL_FILE,    /* GET  /api/classify-file — inference on an SD JPEG */
    IDENT_CLOUD_FILE,    /* GET  /api/cloud-file    — cloud round-trip on an SD JPEG */
    IDENT_CLOUD_TEST,    /* GET  /api/cloud-test    — reachability/key probe */
    IDENT_INAT_TEST,     /* GET  /api/inat-test     — iNat token/reachability probe */
    IDENT_INAT_REFRESH,  /* POST /api/inat-refresh  — mint a fresh JWT from the session cookie */
    IDENT_INAT_LOGIN,    /* POST /api/inat-login    — log in with username/password, then mint a JWT */
    IDENT_INAT_FILE,     /* GET  /api/id-primary    — iNat CV round-trip on an SD JPEG */
    IDENT_INAT_FILE_DBG, /* GET  /api/id-frame-debug — iNat CV round-trip, READ-ONLY (never saves) */
    IDENT_INAT_FILE_DBG_CROP, /* GET /api/id-frame-debug?crop=1 — same, but on a native-res ROI crop */
} ident_kind_t;
static esp_err_t ident_dispatch(httpd_req_t *req, ident_kind_t kind,
                                cloud_provider_t provider,
                                const char *date, const char *file,
                                uint8_t *body, size_t body_len);

/* POST /api/classify — classify a posted JPEG right now (FSD §3.2/§6). Lets
 * users sanity-check their model without waiting for a bird, and is how the
 * classifier is verified end-to-end. The body is received here (network-bound,
 * ~1 s) then handed to the identify worker for the slow decode + inference, so
 * the httpd task isn't held for the inference. */
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
    int total = req->content_len;
    uint8_t *body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int remaining = total, timeout_retries = 0;
    bool ok = true;
    while (remaining > 0) {
        int got = httpd_req_recv(req, (char *) body + (total - remaining), remaining);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries < 5) continue;
            ok = false;
            break;
        }
        timeout_retries = 0;
        remaining -= got;
    }
    if (!ok) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_OK;
    }
    return ident_dispatch(req, IDENT_MODEL_POST, CLOUD_OFF, NULL, NULL, body, total);
}

/* Query parse + path validation shared by the Gallery's two identify
 * endpoints. Rejects anything that could escape /captures/<day>/. */
static bool idfile_params(httpd_req_t *req, char *date, size_t dsz,
                          char *file, size_t fsz)
{
    char query[128] = {0};
    date[0] = file[0] = '\0';
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "date", date, dsz);
    httpd_query_key_value(query, "f", file, fsz);
    url_decode(date);
    url_decode(file);
    return date[0] && file[0] &&
           !strstr(date, "..") && !strchr(date, '/') && !strchr(date, '\\') &&
           !strstr(file, "..") && !strchr(file, '/') && !strchr(file, '\\');
}

/* Look up a saved frame's motion box from the per-day frame-ROI sidecar
 * (/log/frameroi-DATE.csv, "file,x0-y0-x1-y1" per line, §3.4). Used by the crop
 * debug path to centre the crop on the real detection box. False if not found. */
static bool frame_roi_lookup(const char *date, const char *file, roi_t *out)
{
    char path[80];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/frameroi-%.10s.csv", date);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[128];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';
        if (strcmp(line, file) == 0) {
            float a, b, c, d;
            if (sscanf(comma + 1, "%f-%f-%f-%f", &a, &b, &c, &d) == 4) {
                out->x0 = a; out->y0 = b; out->x1 = c; out->y1 = d;
                found = !roi_is_empty(*out);
            }
            break;
        }
    }
    fclose(f);
    return found;
}

/* Persist an identify result as the row's confirmed label (§3.4/v1.58), the
 * same write the ✎ relabel button makes, so it feeds the retrain export
 * (v1.57). A non-empty latin means a real species was decided above the
 * threshold; an under-threshold "Unidentified"/"no bird" answer has an empty
 * latin and writes nothing, so an inconclusive identify never wipes a good
 * row. Shared by the on-device and Claude identify endpoints. */
static bool idfile_save(const char *date, const char *file,
                        const classify_result_t *r, const char *who)
{
    if (!r->latin[0]) return false;
    if (storage_relabel(date, file, r->species, r->latin) != ESP_OK) return false;
    ESP_LOGI(TAG, "%s identify saved %s/%s -> %s", who, date, file, r->species);
    return true;
}

/* ── Async species-ID offload (FSD v1.97) ──────────────────────────────────
 * esp_http_server serves every socket from ONE task, so a handler that ran an
 * on-device inference (~11-20 s) or a Claude round-trip (~1-5 s) inline froze
 * the entire UI for that whole time — a 639-byte /api/status stalled exactly as
 * long as the identify ran (measured). Mirror the /stream fix: duplicate the
 * request, hand it to a worker task, and return so the httpd task goes straight
 * back to serving other sockets.
 *
 * The worker runs BELOW httpd's priority (identify=4 < httpd=5), so even a
 * CPU-bound TFLM inference can't starve request handling. Single-flight: one
 * identify at a time — TFLM callers already serialize on classify.c's run
 * mutex, and the Gallery's per-tile buttons disable themselves in flight and
 * fire one image at a time, so a second concurrent identify is never the fast
 * path. A collision gets 503 {"error":...} (the tile prints it) rather than
 * oversubscribing PSRAM (JPEG buffer + 5.5 MB arena + TLS) and stalling worse. */
typedef struct {
    httpd_req_t *req;         /* async copy — owns the socket until complete */
    ident_kind_t kind;
    cloud_provider_t provider;/* IDENT_CLOUD_*: which cloud provider to call */
    char     date[24];
    char     file[80];
    uint8_t *body;            /* IDENT_MODEL_POST: received JPEG (PSRAM); else NULL */
    size_t   body_len;
} ident_job_t;

static volatile bool s_ident_busy = false;

static void ident_task(void *arg)
{
    ident_job_t *j = (ident_job_t *) arg;
    httpd_req_t *req = j->req;
    classify_result_t r;

    switch (j->kind) {
    case IDENT_MODEL_POST:
        if (classify_run_sync(j->body, j->body_len, &r) != ESP_OK)
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "classification failed");
        else
            classify_send_json(req, &r, false);   /* posted image → no row to save */
        break;

    case IDENT_MODEL_FILE: {
        char path[160];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/captures/%.23s/%.79s",
                 j->date, j->file);
        FILE *f = fopen(path, "rb");
        long sz = 0;
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); }
        if (!f) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        } else if (sz <= 0 || sz > CLASSIFY_MAX_BODY) {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or oversized JPEG");
        } else {
            uint8_t *body = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            size_t rd = body ? fread(body, 1, sz, f) : 0;
            fclose(f);
            if (!body)
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
            else if (rd != (size_t) sz)
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            else if (classify_run_sync(body, sz, &r) != ESP_OK)
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "classification failed");
            else
                classify_send_json(req, &r, idfile_save(j->date, j->file, &r, "model"));
            free(body);
        }
        break;
    }

    case IDENT_CLOUD_FILE: {
        char path[160];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/captures/%.23s/%.79s",
                 j->date, j->file);
        if (cloud_classify_file(j->provider, path, &r) != ESP_OK) {
            /* Surface the provider's own words (bad key, quota, safety block) —
             * the Gallery prints this straight into the tile. */
            char e[128], msg[96];
            json_escape(msg, sizeof(msg), cloud_last_error(j->provider));
            snprintf(e, sizeof(e), "{\"error\":\"%s\"}", msg);
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, e);
        } else {
            classify_send_json(req, &r,
                idfile_save(j->date, j->file, &r, cloud_source_tag(j->provider)));
        }
        break;
    }

    case IDENT_INAT_FILE: {
        char path[160];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/captures/%.23s/%.79s",
                 j->date, j->file);
        if (inat_classify_file(path, &r) != ESP_OK) {
            char e[128], msg[96];
            json_escape(msg, sizeof(msg), inat_last_error());
            snprintf(e, sizeof(e), "{\"error\":\"%s\"}", msg);
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, e);
        } else {
            /* classify_send_json emits latin+common, so a confident iNat ID here
             * also seeds the Gallery's 📋 copy-last (idRun, §3.4/v2.17). */
            classify_send_json(req, &r, idfile_save(j->date, j->file, &r, "inatcv"));
        }
        break;
    }

    case IDENT_INAT_FILE_DBG: {
        /* Read-only reclassify (debug): identical iNat round-trip to id-primary,
         * but it NEVER writes the visit log — no relabel, no confirm, no state
         * change. Lets an event be re-scored to inspect the per-frame result
         * (and top-3 candidate spread, e.g. Pica pica vs the geo-wrong Pica
         * hudsonia) without touching ground truth. `saved` is always false. */
        char path[160];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/captures/%.23s/%.79s",
                 j->date, j->file);
        if (inat_classify_file(path, &r) != ESP_OK) {
            char e[128], msg[96];
            json_escape(msg, sizeof(msg), inat_last_error());
            snprintf(e, sizeof(e), "{\"error\":\"%s\"}", msg);
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, e);
        } else {
            classify_send_json(req, &r, false);   /* read-only: never saved */
        }
        break;
    }

    case IDENT_INAT_FILE_DBG_CROP: {
        /* Read-only A/B (v2.30): re-score iNat on a native-res crop centred on the
         * frame's logged motion box, to test whether a subject-filling crop beats
         * the whole frame on small/off-centre birds. Never saves. */
        roi_t roi;
        if (!frame_roi_lookup(j->date, j->file, &roi)) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"no ROI logged for this frame\"}");
            break;
        }
        char path[160];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/captures/%.23s/%.79s",
                 j->date, j->file);
        FILE *f = fopen(path, "rb");
        long sz = 0;
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); }
        if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); break; }
        uint8_t *buf = (sz > 0 && sz <= CLASSIFY_MAX_BODY)
                     ? heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        size_t rd = buf ? fread(buf, 1, sz, f) : 0;
        fclose(f);
        if (!buf || rd != (size_t) sz) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            break;
        }
        uint8_t *cj = NULL; size_t cl = 0;
        esp_err_t ce = classify_crop_jpeg(buf, sz, roi, &cj, &cl);
        free(buf);
        if (ce != ESP_OK || !cj) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "crop failed");
            break;
        }
        esp_err_t ie = inat_classify_jpeg(cj, cl, &r);
        free(cj);
        if (ie != ESP_OK) {
            char e[128], msg[96];
            json_escape(msg, sizeof(msg), inat_last_error());
            snprintf(e, sizeof(e), "{\"error\":\"%s\"}", msg);
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, e);
        } else {
            classify_send_json(req, &r, false);   /* read-only: never saved */
        }
        break;
    }

    case IDENT_CLOUD_TEST: {
        /* Roomy: the TLS verdict carries an mbedTLS code, the cert-verify flags
         * and the with/without-verification comparison; a clipped one loses
         * exactly the part that says which failure it was. */
        char verdict[288] = "";
        esp_err_t err = cloud_test(j->provider, verdict, sizeof(verdict));
        char msg[384], buf[440];
        json_escape(msg, sizeof(msg), verdict);
        snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", msg);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, buf);
        break;
    }

    case IDENT_INAT_TEST: {
        char verdict[288] = "";
        esp_err_t err = inat_test(verdict, sizeof(verdict));
        char msg[384], buf[440];
        json_escape(msg, sizeof(msg), verdict);
        snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", msg);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, buf);
        break;
    }

    case IDENT_INAT_LOGIN: {
        char verdict[224] = "";
        esp_err_t err = inat_login(verdict, sizeof(verdict));
        char msg[288], buf[352];
        json_escape(msg, sizeof(msg), verdict);
        snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", msg);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, buf);
        break;
    }

    case IDENT_INAT_REFRESH: {
        char verdict[224] = "";
        esp_err_t err = inat_refresh_jwt(verdict, sizeof(verdict));
        char msg[288], buf[352];
        json_escape(msg, sizeof(msg), verdict);
        snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", msg);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, buf);
        break;
    }
    }

    httpd_req_async_handler_complete(req);   /* release the socket */
    free(j->body);
    free(j);
    s_ident_busy = false;
    vTaskDelete(NULL);
}

/* Validate-then-offload front end shared by all four species-ID endpoints.
 * Takes ownership of `body` (freed here on any early bail, else by the worker).
 * Only the single httpd task ever calls this, so the s_ident_busy check/set is
 * race-free against itself; the worker is the only other writer and only ever
 * clears it. */
static esp_err_t ident_dispatch(httpd_req_t *req, ident_kind_t kind,
                                cloud_provider_t provider,
                                const char *date, const char *file,
                                uint8_t *body, size_t body_len)
{
    if (s_ident_busy) {
        free(body);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"identify busy \\u2014 one at a time\"}");
        return ESP_OK;
    }
    ident_job_t *j = calloc(1, sizeof(*j));
    if (!j) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    j->kind = kind;
    j->provider = provider;
    j->body = body;
    j->body_len = body_len;
    if (date) strlcpy(j->date, date, sizeof(j->date));
    if (file) strlcpy(j->file, file, sizeof(j->file));

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        free(body);
        free(j);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
        return ESP_OK;
    }
    j->req = copy;
    s_ident_busy = true;
    if (xTaskCreate(ident_task, "identify", 16384, j, 4, NULL) != pdPASS) {
        s_ident_busy = false;
        httpd_req_async_handler_complete(copy);
        free(body);
        free(j);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory for identify task");
        return ESP_OK;
    }
    return ESP_OK;
}

/* Read a ?p=1|2 provider selector from the query string, defaulting to the
 * given fallback when absent/invalid. Lets the Settings Test button probe a
 * specific provider's key regardless of which one is currently active. */
static cloud_provider_t cloud_provider_param(httpd_req_t *req, cloud_provider_t fallback)
{
    char q[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fallback;
    char v[4];
    if (httpd_query_key_value(q, "p", v, sizeof(v)) != ESP_OK) return fallback;
    if (v[0] == '1') return CLOUD_CLAUDE;
    if (v[0] == '2') return CLOUD_GEMINI;
    return fallback;
}

/* GET /api/cloud-test?p=<1|2> — check reachability + the stored key for a cloud
 * provider without spending tokens (FSD §3.2.3/§6). A deployed box has no serial
 * console, so this is the only way to tell a rejected key from a blocked
 * network. Runs in the identify worker (~1-3 s), so it doesn't hold the httpd
 * task. `p` selects the provider (default: the active one). */
static esp_err_t h_cloud_test(httpd_req_t *req)
{
    cloud_provider_t p = cloud_provider_param(req, cloud_active());
    if (p == CLOUD_OFF) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"pick a provider first\"}");
        return ESP_OK;
    }
    return ident_dispatch(req, IDENT_CLOUD_TEST, p, NULL, NULL, NULL, 0);
}

/* GET /api/inat-test — validate the stored iNaturalist JWT against the free
 * /v1/users/me endpoint (no CV call). A 401 means the ~24 h token expired. Runs
 * in the identify worker so it doesn't hold the httpd task. */
static esp_err_t h_inat_test(httpd_req_t *req)
{
    return ident_dispatch(req, IDENT_INAT_TEST, CLOUD_OFF, NULL, NULL, NULL, 0);
}

/* POST /api/inat-refresh — mint a fresh 24 h JWT from the stored session cookie
 * (v2.36). Offloaded to the identify worker so the ~1-2 s round-trip doesn't
 * block the httpd task, same as the token test. */
static esp_err_t h_inat_refresh(httpd_req_t *req)
{
    return ident_dispatch(req, IDENT_INAT_REFRESH, CLOUD_OFF, NULL, NULL, NULL, 0);
}

/* GET /api/heapdbg — internal-DRAM forensics for the slow leak that kills TLS
 * after ~6 h (§7). /api/sysinfo only reports free bytes and the largest block,
 * which cannot separate a leak from fragmentation. This adds the deciding
 * numbers: `allocBlocks` counts live allocations, so a steady climb IS the leak
 * (and blocks-per-event says how many each event leaks), while allocBlocks flat
 * with largestFree falling means fragmentation instead. `tasks` tests the
 * task-stack theory — FreeRTOS stacks come out of internal DRAM, so a task that
 * never exits shows here (count only; per-task names need
 * CONFIG_FREERTOS_USE_TRACE_FACILITY, which is off). Read-only. */
/* Internal-DRAM block count — the metric that exposes this leak. Free BYTES
 * barely move (the leaked blocks are ~35 B each), so only the count shows it. */
static unsigned dbg_blocks(void)
{
    multi_heap_info_t h = {0};
    heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return (unsigned) h.allocated_blocks;
}

#if CONFIG_HEAP_TRACING
/* GET /api/heaptrace?a=start|stop|dump — standalone heap tracing in LEAKS mode
 * (v2.50). `start` records every allocation with its call stack and DROPS the
 * record when it is freed, so whatever remains after a stop is exactly what
 * leaked. `dump` returns those survivors as JSON — size plus the caller PCs,
 * which resolve to function names off-device with:
 *     xtensa-esp32s3-elf-addr2line -pfiaC -e build/BirdBox.elf <pc> ...
 * 128 records is ample: the hunt is a handful of HTTPS calls, not a whole run.
 * Tracing costs time on every malloc, so it is opt-in and left off. */
#define TRACE_RECORDS 128
static heap_trace_record_t s_trace[TRACE_RECORDS];
static bool s_trace_ready = false;

static esp_err_t h_heaptrace(httpd_req_t *req)
{
    char q[48] = {0}, a[12] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "a", a, sizeof(a));

    if (strcmp(a, "start") == 0) {
        if (!s_trace_ready) {
            if (heap_trace_init_standalone(s_trace, TRACE_RECORDS) != ESP_OK) {
                httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"trace init failed\"}");
                return ESP_OK;
            }
            s_trace_ready = true;
        }
        esp_err_t e = heap_trace_start(HEAP_TRACE_LEAKS);
        char b[64];
        snprintf(b, sizeof(b), "{\"ok\":%s,\"mode\":\"leaks\"}", e == ESP_OK ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, b);
        return ESP_OK;
    }
    if (strcmp(a, "stop") == 0) {
        esp_err_t e = heap_trace_stop();
        char b[80];
        snprintf(b, sizeof(b), "{\"ok\":%s,\"live\":%u}",
                 e == ESP_OK ? "true" : "false", (unsigned) heap_trace_get_count());
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, b);
        return ESP_OK;
    }
    if (strcmp(a, "dump") == 0) {
        size_t n = heap_trace_get_count();
        httpd_resp_set_type(req, "application/json");
        char b[224];
        snprintf(b, sizeof(b), "{\"live\":%u,\"records\":[", (unsigned) n);
        httpd_resp_send_chunk(req, b, strlen(b));
        bool first = true;
        for (size_t i = 0; i < n; i++) {
            heap_trace_record_t r;
            if (heap_trace_get(i, &r) != ESP_OK) continue;
            int o = snprintf(b, sizeof(b), "%s{\"size\":%u,\"addr\":\"0x%08x\",\"pc\":[",
                             first ? "" : ",", (unsigned) r.size, (unsigned) (uintptr_t) r.address);
            first = false;
            for (int k = 0; k < CONFIG_HEAP_TRACING_STACK_DEPTH; k++) {
                if (!r.alloced_by[k]) break;
                o += snprintf(b + o, sizeof(b) - o, "%s\"0x%08x\"", k ? "," : "",
                              (unsigned) (uintptr_t) r.alloced_by[k]);
            }
            o += snprintf(b + o, sizeof(b) - o, "]}");
            httpd_resp_send_chunk(req, b, o);
        }
        httpd_resp_send_chunk(req, "]}", 2);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a=start|stop|dump");
    return ESP_OK;
}

#endif /* CONFIG_HEAP_TRACING */

/* GET /api/heapdbg?probe=1 — run ONE outbound HTTPS request stage by stage and
 * report the live block count after each, so the leak is attributed to a
 * specific call instead of guessed at. Stages: init, set_header x4, open,
 * fetch+read, close, cleanup. A well-behaved client returns to `b0` at the end;
 * whatever the residual is, and wherever it first appears, is the bug. */
static void tls_probe(char *out, size_t osz)
{
    unsigned b0 = dbg_blocks();
    esp_http_client_config_t cfg = {
        .url               = "https://api.inaturalist.org/v1/users/me",
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    unsigned b1 = dbg_blocks();
    if (!c) { snprintf(out, osz, "\"probe\":\"init failed\""); return; }
    esp_http_client_set_header(c, "User-Agent", "BirdBox-probe");
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "X-Probe-A", "1");
    esp_http_client_set_header(c, "X-Probe-B", "2");
    unsigned b2 = dbg_blocks();
    esp_err_t e = esp_http_client_open(c, 0);
    unsigned b3 = dbg_blocks();
    int status = 0;
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        char sink[256];
        while (esp_http_client_read(c, sink, sizeof(sink)) > 0) { }
        status = esp_http_client_get_status_code(c);
    }
    unsigned b4 = dbg_blocks();
    esp_http_client_close(c);
    unsigned b5 = dbg_blocks();
    esp_http_client_cleanup(c);
    unsigned b6 = dbg_blocks();
    snprintf(out, osz,
             "\"probe\":{\"open\":\"%s\",\"status\":%d,"
             "\"b0_start\":%u,\"b1_init\":%u,\"b2_headers\":%u,\"b3_open\":%u,"
             "\"b4_read\":%u,\"b5_close\":%u,\"b6_cleanup\":%u,\"residual\":%d}",
             esp_err_to_name(e), status, b0, b1, b2, b3, b4, b5, b6,
             (int) b6 - (int) b0);
}

static esp_err_t h_heapdbg(httpd_req_t *req)
{
    multi_heap_info_t in = {0}, ps = {0};
    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT);
    char buf[900];   /* base JSON ~300 B; ?probe=1 appends ~300 B more */
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime\":%lu,\"tasks\":%u,"
        "\"int\":{\"free\":%u,\"alloc\":%u,\"largestFree\":%u,\"minFree\":%u,"
        "\"allocBlocks\":%u,\"freeBlocks\":%u,\"totalBlocks\":%u},"
        "\"psram\":{\"free\":%u,\"largestFree\":%u,\"allocBlocks\":%u},"
        "\"events\":%lu}",
        (unsigned long) (esp_timer_get_time() / 1000000),
        (unsigned) uxTaskGetNumberOfTasks(),
        (unsigned) in.total_free_bytes, (unsigned) in.total_allocated_bytes,
        (unsigned) in.largest_free_block, (unsigned) in.minimum_free_bytes,
        (unsigned) in.allocated_blocks, (unsigned) in.free_blocks,
        (unsigned) in.total_blocks,
        (unsigned) ps.total_free_bytes, (unsigned) ps.largest_free_block,
        (unsigned) ps.allocated_blocks,
        (unsigned long) capture_event_count());

    /* ?probe=1 appends a staged HTTPS run (several seconds — it goes to the
     * network, so it is opt-in and never part of a plain heapdbg poll). */
    char q[48] = {0}, pv[8] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "probe", pv, sizeof(pv)) == ESP_OK && pv[0] == '1' &&
        n > 0 && n < (int) sizeof(buf) - 320) {
        char pr[300] = "";
        tls_probe(pr, sizeof(pr));
        n--;                                   /* drop the closing brace */
        n += snprintf(buf + n, sizeof(buf) - n, ",%s}", pr);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* GET /api/frameroi?date=<day>&f=<file> — report the motion box the frame-ROI
 * sidecar holds for one frame, resolved through the SAME storage_frameroi_*
 * helpers the recheck path uses (v2.47). Read-only diagnostic: it answers "does
 * recheck actually get an ROI for this frame?" without a serial console. */
static esp_err_t h_frameroi(httpd_req_t *req)
{
    char date[16] = "", file[80] = "";
    if (!idfile_params(req, date, sizeof(date), file, sizeof(file))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad args");
        return ESP_OK;
    }
    char *buf = storage_frameroi_load(date);
    char roi[24] = "";
    bool found = buf && storage_frameroi_find(buf, file, roi, sizeof(roi));
    char out[160];
    snprintf(out, sizeof(out), "{\"sidecar\":%s,\"found\":%s,\"roi\":\"%s\"}",
             buf ? "true" : "false", found ? "true" : "false", roi);
    free(buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* POST /api/inat-login — log in with the stored username/password and mint a JWT
 * (v2.43). Two round-trips to inaturalist.org, so it goes to the identify worker
 * like the refresh above. Credentials come from Settings, never from this body. */
static esp_err_t h_inat_login(httpd_req_t *req)
{
    return ident_dispatch(req, IDENT_INAT_LOGIN, CLOUD_OFF, NULL, NULL, NULL, 0);
}

/* GET /api/cloud-file?date=<day>&f=<file> — identify one saved photo with the
 * cloud (FSD §3.2.3/§6). The Gallery's ✨ button: the point is the §3.2.2
 * retrain loop, where confirming a strong cloud label with one click beats
 * typing a species name per image. Uses the active provider, or — if none is
 * selected — whichever one has a key (cloud_for_manual), so the button works
 * with the live path off. Independent of the provider selector otherwise.
 *
 * Offloaded to the identify worker for the round-trip (~1-5 s) so the httpd
 * task isn't held — see ident_dispatch. */
static esp_err_t h_cloud_file(httpd_req_t *req)
{
    cloud_provider_t p = cloud_for_manual();
    if (p == CLOUD_OFF) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"error\":\"no cloud API key (Settings \\u2192 Cloud Identification)\"}");
        return ESP_OK;
    }
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD card");
        return ESP_OK;
    }
    char date[24] = {0}, file[80] = {0};
    if (!idfile_params(req, date, sizeof(date), file, sizeof(file))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    return ident_dispatch(req, IDENT_CLOUD_FILE, p, date, file, NULL, 0);
}

/* GET /api/classify-file?date=<day>&f=<file> — classify a JPEG already on the
 * SD card (FSD §3.2/§6). Lets the Gallery re-run species ID on a saved photo
 * without re-uploading it over WiFi: the device reads the file itself. The read
 * + decode + inference (~seconds) run in the identify worker, so the httpd task
 * isn't held — see ident_dispatch. */
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
    char date[24] = {0}, file[80] = {0};
    if (!idfile_params(req, date, sizeof(date), file, sizeof(file))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    return ident_dispatch(req, IDENT_MODEL_FILE, CLOUD_OFF, date, file, NULL, 0);
}

/* GET /api/id-primary?date=<day>&f=<file> — identify a saved photo with the
 * PRIMARY classifier: iNaturalist online CV when enabled (Settings), else the
 * on-device model. This is the Gallery's 🔍 button, so a manual "identify" uses
 * the same engine that leads the live cascade. A confident result feeds the
 * copy-last fast-path (classify_send_json emits latin+common → idRun). */
static esp_err_t h_id_primary(httpd_req_t *req)
{
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD card");
        return ESP_OK;
    }
    char date[24] = {0}, file[80] = {0};
    if (!idfile_params(req, date, sizeof(date), file, sizeof(file))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    if (inat_cv_enabled())
        return ident_dispatch(req, IDENT_INAT_FILE, CLOUD_OFF, date, file, NULL, 0);
    if (!classify_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"error\":\"no classifier (no model on SD, and iNaturalist off)\"}");
        return ESP_OK;
    }
    return ident_dispatch(req, IDENT_MODEL_FILE, CLOUD_OFF, date, file, NULL, 0);
}

/* GET /api/id-frame-debug?date=<day>&f=<file> — re-run iNaturalist CV on a saved
 * frame and return the result WITHOUT saving it (no relabel/confirm/state change).
 * Purely for debugging: redo an "unclassified" or geo-wrong event to inspect what
 * iNat says per frame (species + top-3 candidates) without mutating ground truth.
 * Requires iNat online to be enabled (there is no read-only debug for the other
 * tiers — cloud costs money, the model has /api/classify-file). */
static esp_err_t h_id_frame_debug(httpd_req_t *req)
{
    if (!storage_sd_present()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD card");
        return ESP_OK;
    }
    if (!inat_cv_enabled()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"iNaturalist online is off\"}");
        return ESP_OK;
    }
    char date[24] = {0}, file[80] = {0};
    if (!idfile_params(req, date, sizeof(date), file, sizeof(file))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    char q[160] = {0}, crop[4] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "crop", crop, sizeof(crop));
    ident_kind_t kind = (crop[0] == '1') ? IDENT_INAT_FILE_DBG_CROP : IDENT_INAT_FILE_DBG;
    return ident_dispatch(req, kind, CLOUD_OFF, date, file, NULL, 0);
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
    if (s_httpd) return ESP_OK;   /* already running (portal path starts us early) */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 64;     /* headroom above route count (56 as of v2.50) — an
                                      exact-fit cap silently drops later registrations,
                                      404ing whichever routes land last (RemoteStart v1.27;
                                      hit again at v1.51 when the count crossed 32) */
    cfg.stack_size       = 8192;
    cfg.max_open_sockets  = HTTPD_MAX_SOCKETS;   /* 12, up from default 7 — pool headroom
                                      so a fresh request doesn't stall purging a stale
                                      socket (FSD v2.13; needs CONFIG_LWIP_MAX_SOCKETS>=15) */
    cfg.recv_wait_timeout = 3;     /* down from default 5s: a stale/idle socket is
                                      reclaimed in ~3s not 5s, shrinking the worst-case
                                      first-request-after-idle stall. Still ample for a
                                      continuous OTA/model upload (chunks arrive in ms) */
    cfg.send_wait_timeout = 3;
    cfg.lru_purge_enable = true;   /* abandoned sessions must not exhaust the socket
                                      pool and wedge the server (RemoteStart v1.35) */
    cfg.uri_match_fn     = httpd_uri_match_wildcard;   /* for the /captures wildcard route */

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
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
        { .uri = "/api/relabel-batch", .method = HTTP_POST, .handler = h_relabel_batch },
        { .uri = "/api/roi-todo", .method = HTTP_GET,  .handler = h_roi_todo },
        { .uri = "/api/set-roi",  .method = HTTP_POST, .handler = h_set_roi  },
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
        { .uri = "/ota/from-url", .method = HTTP_POST, .handler = h_ota_from_url },
        { .uri = "/ota/from-url", .method = HTTP_GET,  .handler = h_ota_from_url_status },
        { .uri = "/api/classify",  .method = HTTP_POST, .handler = h_classify_run },
        { .uri = "/api/classify-file", .method = HTTP_GET, .handler = h_classify_file },
        { .uri = "/api/id-primary", .method = HTTP_GET, .handler = h_id_primary },
        { .uri = "/api/id-frame-debug", .method = HTTP_GET, .handler = h_id_frame_debug },
        { .uri = "/api/cloud-file", .method = HTTP_GET, .handler = h_cloud_file },
        { .uri = "/api/cloud-test", .method = HTTP_GET, .handler = h_cloud_test },
        { .uri = "/api/inat-test", .method = HTTP_GET, .handler = h_inat_test },
        { .uri = "/api/inat-refresh", .method = HTTP_POST, .handler = h_inat_refresh },
        { .uri = "/api/inat-login",   .method = HTTP_POST, .handler = h_inat_login   },
        { .uri = "/api/frameroi",     .method = HTTP_GET,  .handler = h_frameroi     },
        { .uri = "/api/heapdbg",      .method = HTTP_GET,  .handler = h_heapdbg      },
#if CONFIG_HEAP_TRACING
        { .uri = "/api/heaptrace",    .method = HTTP_GET,  .handler = h_heaptrace    },
#endif
        { .uri = "/model/upload",  .method = HTTP_POST, .handler = h_model_upload },
        { .uri = "/model/delete",  .method = HTTP_POST, .handler = h_model_delete },
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(s_httpd, &routes[i]);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Failed to register %s: %s", routes[i].uri, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Web server started (%u routes)",
             (unsigned) (sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
