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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
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

static const char *TAG = "web";

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
"<img class='live' id='live' src='/stream' alt='live stream'"
" onerror=\"this.alt='no camera / stream unavailable';\">"
"</div>"
"<div class='sts' id='sts'></div>"
"<button class='act' onclick='snap()'>&#128247; Snapshot to SD</button>"
"</div>"
"<div id='galleryp' class='pane'>"
"<div class='gbar'><select id='day' onchange='loadDay()'></select>"
"<button class='act' style='margin:0' onclick='loadDays()'>&#8635; Refresh</button>"
"<button class='act' style='margin:0' onclick='gSelAll()'>&#9745; Select all</button>"
"<button class='act' style='margin:0' onclick='gDelSel()'>&#10060; Delete selected</button>"
"<button class='act' style='margin:0;background:#8a3f3f' onclick='gDelAll()'>&#128465; Delete all</button>"
"<button class='act' style='margin:0;background:#9e3030' onclick='gWipeDay()'>&#9888; Wipe day (photos+stats)</button>"
"<span class='sts' id='gsts' style='margin:0'></span>"
"<span class='sts' id='gselc' style='margin:0;color:#7fc98b'></span></div>"
"<div class='grid' id='grid'></div>"
"</div>"
"<div id='statsp' class='pane'>"
"<h3 class='sh' style='margin-top:0'>Visits per day</h3><div id='sDaily'></div>"
"<h3 class='sh'>Activity by hour of day</h3><div class='hwrap' id='sHourly'></div>"
"<h3 class='sh'>Species</h3><table class='st' id='sSpecies'></table>"
"<div class='sts' id='sTotal'></div>"
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
"<label class='wl'>Resolution (higher = more detail, needs reboot)</label>"
"<select class='wi' id='stRes'>"
"<option value='0'>VGA 640&times;480</option><option value='1'>SVGA 800&times;600</option>"
"<option value='2'>XGA 1024&times;768</option><option value='3'>HD 1280&times;720</option>"
"<option value='4'>SXGA 1280&times;1024</option>"
"</select>"
"<label class='wl'>Contrast (the OV2640 has no sharpness control)</label>"
"<select class='wi' id='stContrast'>"
"<option value='-2'>-2 (soft)</option><option value='-1'>-1</option>"
"<option value='0'>0 (default)</option><option value='1'>+1</option>"
"<option value='2'>+2 (punchy)</option></select>"
"<p class='sts' style='margin-top:2px'>Resolution takes effect after a reboot; contrast "
"applies immediately. Higher resolution uses more memory and slows species ID.</p>"
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
"<label class='wl'>IR LED</label>"
"<select class='wi' id='stIr'>"
"<option value='0'>Off</option><option value='1'>Auto (needs IR hardware)</option></select>"
"<p class='sts'>Settings apply immediately &mdash; no reboot needed.</p>"
"<button class='act' style='margin-left:0' onclick='stSave()'>&#128190; Save Settings</button>"
"<span class='sts' id='stSts'></span>"
"</div>"
"<div id='dbgp' class='pane'>"
"<h3 class='sh' style='margin-top:0'>System</h3><div id='dSys'></div>"
"<h3 class='sh'>WiFi Link</h3><div id='dWifi'></div>"
"<h3 class='sh'>SD Card</h3><div id='dSd'></div>"
"<h3 class='sh'>Camera</h3><div id='dCam'></div>"
"<h3 class='sh'>Species ID</h3><div id='dCls'></div>"
"<button class='act' style='margin-left:0' onclick='loadDebug()'>&#8635; Refresh</button>"
"</div>"
"<div id='wifip' class='pane'>"
"<h3 class='sh' style='margin-top:0'>WiFi Network</h3>"
"<button class='act' style='margin:0 0 6px' onclick='wfScan()'>&#128246; Scan Networks</button>"
"<div class='sts' id='wfSts'></div><div id='wfNets'></div>"
"<form action='/wifi-save' method='POST'>"
"<label class='wl'>SSID</label>"
"<input class='wi' id='wfSid' name='ssid' placeholder='tap network or type' required>"
"<label class='wl'>Password</label>"
"<input class='wi' name='pass' type='password' placeholder='leave blank if open'>"
"<br><button class='act' type='submit'>Connect &amp; Save</button></form>"
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
"if(id==='galleryp')loadDays();"
"if(id==='statsp')loadStats();"
"if(id==='setp')stLoad();"
"if(id==='dbgp')loadDebug();"
"if(id==='wifip')ipLoad();}"
"function tick(){fetch('/api/status').then(r=>r.json()).then(s=>{"
"var t=s.ip+' | RSSI '+s.rssi+' dBm | heap '+Math.round(s.heap/1024)+' KB | up '+s.uptime+' s'"
"+' | SD '+(s.sdPresent?s.sdFreeMB+' MB free':'none')"
"+' | motion '+(s.motion?'ACTIVE':'idle')+' | events '+s.events;"
"if(s.lastEvent)t+=' | last: <a href=\"'+s.lastEvent+'\">'+s.lastEvent.split('/').pop()+'</a>';"
"if(s.species)t+=' ('+s.species+')';"
"document.getElementById('sts').innerHTML=t;"
"}).catch(()=>{});}tick();setInterval(tick,2000);"
"function snap(){fetch('/api/capture',{method:'POST'}).then(r=>r.json())"
".then(o=>{alert(o.path?('Saved '+o.path):JSON.stringify(o));}).catch(()=>alert('failed'));}"
"function applyRot(v){v=String(v);var w=$g('liveWrap'),l=$g('live');"
"w.classList.remove('r1','r3');l.classList.remove('r1','r3');"
"if(v==='1'){w.classList.add('r1');l.classList.add('r1');}"
"if(v==='3'){w.classList.add('r3');l.classList.add('r3');}}"
"function lvRotChange(){var v=$g('lvRot').value;applyRot(v);"
"if($g('stRot'))$g('stRot').value=v;"
"fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'rot='+v});}"
"function loadRot(){fetch('/api/settings').then(r=>r.json()).then(function(c){"
"$g('lvRot').value=c.rot;applyRot(c.rot);}).catch(()=>{});}"
"loadRot();"
"function loadDays(){fetch('/api/days').then(r=>r.json()).then(a=>{"
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
"var lab=a.filter(o=>o.sp).length;"
"$g('gsts').textContent=a.length+' capture'+(a.length!==1?'s':'')"
"+(lab?(' \\u00b7 '+lab+' labelled'):'');"
"var g=$g('grid');g.innerHTML='';"
"a.forEach(o=>{var p='/captures/'+d+'/'+o.f;"
"var div=document.createElement('div');div.className='gitem';"
"var bdg=o.sp?('<div class=\"glabel\" title=\"'+esc(o.sp)+' '+(o.pct||0)+'%\">'"
"+esc(o.sp)+' '+(o.pct||0)+'%</div>'):'';"
"div.innerHTML=bdg+'<input type=\"checkbox\" class=\"gchk\" data-f=\"'+esc(o.f)+'\" "
"onchange=\"gSelSync(this)\">"
"<a href=\"'+p+'\" target=\"_blank\"><img loading=\"lazy\" src=\"'+p+'\"></a>"
"<div class=\"gmeta\"><span>'+o.f+' &middot; '+Math.round(o.s/1024)+' KB</span>"
"<button title=\"delete\" onclick=\"del(\\''+p+'\\')\">&#10060;</button></div>';"
"g.appendChild(div);});gSelSync();});}"
"function gChecks(){return [...document.querySelectorAll('#grid .gchk')];}"
"function gSelSync(cb){if(cb)cb.closest('.gitem').classList.toggle('sel',cb.checked);"
"var n=gChecks().filter(c=>c.checked).length;"
"$g('gselc').textContent=n?(n+' selected'):'';}"
"function gSelAll(){var c=gChecks();var on=c.some(x=>!x.checked);"
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
"function loadStats(){"
"Promise.all(['daily','species','hourly'].map(u=>fetch('/api/stats/'+u).then(r=>r.json())))"
".then(function(res){var d=res[0],sp=res[1],h=res[2];"
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
"sp.map(o=>'<tr><td>'+o.s+'</td><td>'+o.n+'</td><td>'+o.first+'</td><td>'+o.last+'</td></tr>').join('');"
"document.getElementById('sTotal').textContent=sp.reduce((a,o)=>a+o.n,0)+' visits total';"
"});}"
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
"function wfScan(){var st=document.getElementById('wfSts');"
"st.textContent='Scanning\\u2026';document.getElementById('wfNets').innerHTML='';"
"fetch('/api/scan').then(r=>r.json()).then(function(a){"
"st.textContent=a.length+' network'+(a.length!==1?'s':'')+' found';"
"var c=document.getElementById('wfNets');"
"a.forEach(function(n){var d=document.createElement('div');d.className='wnet';"
"d.innerHTML='<span>'+n.ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</span>"
"<span class=sts style=\"margin:0\">'+n.rssi+'dBm'+(n.auth?' &#128274;':'')+'</span>';"
"d.onclick=function(){document.getElementById('wfSid').value=n.ssid;};"
"c.appendChild(d);});}).catch(function(){st.textContent='Scan failed';});}"
"function rebootDev(){if(confirm('Reboot BirdBox?'))"
"fetch('/api/reboot',{method:'POST'}).then(()=>alert('Rebooting\\u2026'));}"
"function $g(i){return document.getElementById(i)}"
"var g_savedRes=1;"
"function stSensShow(){$g('stSensV').textContent=$g('stSens').value;}"
"function stLoad(){fetch('/api/settings').then(r=>r.json()).then(function(c){"
"(c.mode===1?$g('stFeed'):$g('stNest')).checked=true;"
"$g('stSens').value=c.sens;stSensShow();"
"$g('stCcnt').value=c.ccnt;$g('stCivl').value=c.civl;$g('stCool').value=c.cool;"
"$g('stConf').value=c.conf;$g('stCap').value=c.cap;$g('stIr').value=c.ir;"
"$g('stLang').value=c.lang;"
"$g('stRot').value=c.rot;$g('lvRot').value=c.rot;applyRot(c.rot);"
"$g('stRfilt').value=c.rfilt;"
"$g('stRes').value=c.res;$g('stContrast').value=c.contrast;g_savedRes=c.res;"
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
"+'&conf='+$g('stConf').value+'&cap='+$g('stCap').value"
"+'&rfilt='+$g('stRfilt').value"
"+'&qual='+$g('stQual').value+'&ir='+$g('stIr').value"
"+'&rot='+$g('stRot').value+'&res='+$g('stRes').value"
"+'&contrast='+$g('stContrast').value"
"+'&tz='+encodeURIComponent($g('stTz').value)"
"+'&region='+encodeURIComponent($g('stRegion').value)"
"+'&ntp='+encodeURIComponent(ntp)"
"+'&lang='+$g('stLang').value;"
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
"drow('WiFi reconnects',d.wifiDisc)+"
"drow('Last reconnect',fmtAge(d.wifiDiscAgo)+(d.wifiDiscAgo<0?'':' ago'));"
"$g('dWifi').innerHTML="
"drow('RSSI',d.rssi+' dBm')+drow('Channel',d.ch)+drow('Own MAC',d.mac);"
"$g('dSd').innerHTML=d.sdPresent?"
"drow('Card',d.sdCard)+drow('Size',d.sdTotalMB+' MB total, '+d.sdFreeMB+' MB free')+"
"drow('Last write',d.sdWriteOk?'OK':'FAILED',d.sdWriteOk?'ok':'bad'):"
"drow('Status','no SD card','bad');"
"$g('dCam').innerHTML=d.camPresent?"
"drow('Sensor PID','0x'+d.camPid.toString(16))+drow('Resolution',d.camRes)+"
"drow('JPEG quality',d.camQuality):drow('Status','no camera','bad');"
"$g('dCls').innerHTML=d.clsModel?"
"drow('Model',d.clsModel)+drow('Labels',d.clsLabels+' species')+"
"drow('Northern-Europe species',(d.clsRegion||0)+' of '+d.clsLabels"
"+((d.clsRegion||0)?'':' \\u2014 region filter N/A for this model'))+"
"drow('Last inference',d.lastInferenceMs<0?'none yet':d.lastInferenceMs+' ms'):"
"drow('Status','no model loaded \\u2014 upload a .tflite + .txt to /sd/model','bad');"
"}).catch(()=>{});}"
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

/* Gallery event labels (§3.4): the visit log records each event's first frame
 * + species, so join that to the day's image files to badge event thumbnails
 * with their identification. Only first frames match (one log row per event);
 * follow-up frames stay unlabeled. */
#define GAL_MAX_LABELS 300
typedef struct { char base[16]; char sp[72]; uint8_t pct; } gal_label_t;

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
    char line[224];
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
        if (corrected[0]) { species = corrected; latin = (char *) ""; }
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

    gal_label_t *labels = calloc(GAL_MAX_LABELS, sizeof(gal_label_t));
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
        for (int i = 0; i < nlabels; i++)
            if (strcmp(labels[i].base, e->d_name) == 0) {
                sp = labels[i].sp; pct = labels[i].pct; break;
            }
        char item[256];
        int len;
        if (sp)
            len = snprintf(item, sizeof(item),
                           "%s{\"f\":\"%.48s\",\"s\":%ld,\"sp\":\"%s\",\"pct\":%d}",
                           first ? "" : ",", e->d_name, (long) st.st_size, sp, pct);
        else
            len = snprintf(item, sizeof(item), "%s{\"f\":\"%.48s\",\"s\":%ld}",
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
    httpd_resp_send_chunk(req, "[", 1);
    for (int i = 0; i < st->sp_count; i++) {
        char species[80];
        species_localize(st->sp[i], st->sp_latin[i], g_settings.lang,
                          species, sizeof(species));
        char item[192];
        int len = snprintf(item, sizeof(item),
                           "%s{\"s\":\"%s\",\"n\":%u,\"first\":\"%s\",\"last\":\"%s\"}",
                           i ? "," : "", species, st->sp_n[i],
                           st->sp_first[i], st->sp_last[i]);
        httpd_resp_send_chunk(req, item, len);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    free(st);
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
    char buf[448];
    int n = snprintf(buf, sizeof(buf),
        "{\"mode\":%d,\"sens\":%u,\"ccnt\":%u,\"civl\":%u,\"cool\":%u,"
        "\"conf\":%u,\"cap\":%u,\"qual\":%u,\"ir\":%u,\"rot\":%u,\"rfilt\":%u,"
        "\"res\":%u,\"contrast\":%d,\"tz\":\"%s\","
        "\"region\":\"%s\",\"ntp\":\"%s\",\"lang\":%u,\"models\":[",
        g_settings.mode, g_settings.motion_sensitivity, g_settings.capture_count,
        g_settings.capture_interval_ms, g_settings.cooldown_s,
        g_settings.confidence_pct, g_settings.sd_cap_pct,
        g_settings.stream_quality, g_settings.ir_led_mode, (unsigned) g_settings.rotation,
        (unsigned) g_settings.region_filter,
        (unsigned) g_settings.resolution, (int) g_settings.contrast,
        g_settings.timezone, g_settings.region, g_settings.ntp_server,
        (unsigned) g_settings.lang);
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
    char body[512] = {0};
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

    settings_save();

    /* Apply live — no reboot (FSD §5) */
    setenv("TZ", g_settings.timezone, 1);
    tzset();
    camera_set_quality(g_settings.stream_quality);   /* no-op without camera */
    camera_set_rotation(g_settings.rotation);        /* no-op without camera */
    camera_set_contrast(g_settings.contrast);        /* no-op without camera */
    /* resolution is applied at camera_init — needs a reboot (FSD §5) */
    wifi_restart_sntp();                             /* no-op before first connect */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
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

    char buf[560];
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"version\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"heap\":%lu,\"uptime\":%lld,\"portal\":%s,\"wifiReconnects\":%lu,"
        "\"sdPresent\":%s,\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,"
        "\"motion\":%s,\"events\":%lu,\"lastEvent\":\"%s\",\"species\":\"%s\"}",
        FIRMWARE_NAME, FIRMWARE_VERSION, ip, rssi, ch,
        (unsigned long) esp_get_free_heap_size(),
        esp_timer_get_time() / 1000000,
        wifi_in_portal_mode() ? "true" : "false",
        (unsigned long) g_wifi_disconnect_count,
        storage_sd_present() ? "true" : "false",
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        motion_active() ? "true" : "false",
        (unsigned long) capture_event_count(),
        capture_last_event_path(),
        classify_last_species()[0] ? species : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
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

    uint64_t sd_total, sd_free;
    storage_get_info(&sd_total, &sd_free);
    char card[24];
    storage_get_card_name(card, sizeof(card));

    int64_t now_us = esp_timer_get_time();

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"heap\":%lu,\"heapMin\":%lu,\"heapMinAgo\":%lld,\"uptime\":%lld,"
        "\"wifiDisc\":%lu,\"wifiDiscAgo\":%lld,\"mac\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"sdPresent\":%s,\"sdCard\":\"%s\",\"sdTotalMB\":%llu,\"sdFreeMB\":%llu,"
        "\"sdWriteOk\":%s,"
        "\"camPresent\":%s,\"camPid\":%d,\"camRes\":\"%s\",\"camQuality\":%u,"
        "\"lastInferenceMs\":%ld,\"clsModel\":\"%s\",\"clsLabels\":%d,\"clsRegion\":%d}",
        (unsigned long) esp_get_free_heap_size(),
        (unsigned long) g_heap_min,
        (long long) ((now_us - g_heap_min_ts_us) / 1000000),
        now_us / 1000000,
        (unsigned long) g_wifi_disconnect_count,
        g_wifi_last_disc_ts_us ? (long long) ((now_us - g_wifi_last_disc_ts_us) / 1000000) : -1,
        mac_str, rssi, ch,
        storage_sd_present() ? "true" : "false", card,
        sd_total / (1024 * 1024), sd_free / (1024 * 1024),
        storage_last_write_ok() ? "true" : "false",
        camera_available() ? "true" : "false", camera_get_pid(),
        camera_framesize_str(), g_settings.stream_quality,
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

    char species[80];
    species_localize(r.species, r.latin, g_settings.lang, species, sizeof(species));

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"species\":\"%s\",\"confidence\":%u,\"durationMs\":%ld,\"top3\":["
        "{\"label\":\"%s\",\"pct\":%u},{\"label\":\"%s\",\"pct\":%u},"
        "{\"label\":\"%s\",\"pct\":%u}]}",
        species, r.confidence_pct, (long) r.duration_ms,
        r.top_label[0], r.top_pct[0], r.top_label[1], r.top_pct[1],
        r.top_label[2], r.top_pct[2]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
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
    cfg.max_uri_handlers = 28;     /* headroom above route count — an exact-fit cap
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
        { .uri = "/api/stats/daily",   .method = HTTP_GET, .handler = h_stats_daily   },
        { .uri = "/api/stats/species", .method = HTTP_GET, .handler = h_stats_species },
        { .uri = "/api/stats/hourly",  .method = HTTP_GET, .handler = h_stats_hourly  },
        { .uri = "/api/stats/reset",   .method = HTTP_POST, .handler = h_stats_reset  },
        { .uri = "/api/ipconfig",      .method = HTTP_GET,  .handler = h_ipcfg_get  },
        { .uri = "/api/ipconfig/save", .method = HTTP_POST, .handler = h_ipcfg_save },
        { .uri = "/api/settings",      .method = HTTP_GET,  .handler = h_settings_get  },
        { .uri = "/api/settings",      .method = HTTP_POST, .handler = h_settings_post },
        { .uri = "/api/sysinfo",       .method = HTTP_GET,  .handler = h_sysinfo    },
        { .uri = "/api/captures/delete", .method = HTTP_POST, .handler = h_captures_delete_batch },
        { .uri = "/captures/*",  .method = HTTP_GET,  .handler = h_captures_file },
        { .uri = "/captures/*",  .method = HTTP_DELETE, .handler = h_captures_delete },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = h_reboot     },
        { .uri = "/ota/upload",  .method = HTTP_POST, .handler = h_ota_upload },
        { .uri = "/api/classify",  .method = HTTP_POST, .handler = h_classify_run },
        { .uri = "/model/upload",  .method = HTTP_POST, .handler = h_model_upload },
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
