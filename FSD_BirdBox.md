# Functional Specification Document
## BirdBox — WiFi Nest Box / Feeder Camera with AI Species Identification
**Version:** 1.10
**Author:** SEspe
**Date:** 2026-07-07
**Changelog:**
- v1.10 — **Debug tab implemented (firmware 0.9.0), §5 now fully live; new `GET /api/sysinfo`.** Mirrors the RemoteStart Debug-card convention: a `housekeeping_task` in `main.c` samples `esp_get_free_heap_size()` every 10 s and tracks the lifetime low-water mark plus when it was hit (`g_heap_min`/`g_heap_min_ts_us`), so a slow leak shows up as an aging minimum long before the device actually runs low. Debug tab renders five cards from the one endpoint: **System** (free heap, low-water mark + age, uptime, WiFi reconnect count + last-reconnect age from the existing `g_wifi_disconnect_count`/`g_wifi_last_disc_ts_us`), **WiFi Link** (RSSI, channel, own MAC via `esp_wifi_get_mac`), **SD Card** (card CID name, size/free, and a **last-write-ok flag** as the closest thing to a "health" signal FAT/SDMMC exposes — no SMART equivalent exists for SD, so `storage.c` now tracks whether the most recent capture or visit-log write actually succeeded), **Camera** (sensor PID, resolution, current JPEG quality), and **Species ID** — which honestly reports "not available yet" via a new `classify_last_duration_ms()` stub (always -1) rather than fabricating a number ahead of §3.2. Verified live: heap/uptime/reconnect fields sane on the reference unit, SD card fields matched the mounted 64 GB card, camera card showed the correct sensor PID and live-adjustable quality, no-camera/no-SD states render their "bad" (red) status correctly. Firmware 0.9.0.
- v1.9 — **Settings tab implemented (firmware 0.8.0), §5 now live; NTP server made configurable.** `settings.c` moves off the scaffold: every field gets its own NVS key (not a blob) so a future new field falls back to its default instead of invalidating every stored setting on a version bump. Settings tab exposes placement mode, motion sensitivity/frames/interval/cool-down, confidence threshold, SD retention cap, stream/capture JPEG quality (`camera_set_quality()` re-programs the sensor live, no reboot), timezone, IR LED mode, and a **region selector for the future species model (§3.2)** — options are read live from whatever files sit in `/sd/model` (`GET /api/settings` lists them), so the region choice is future-proofed for the model-swap workflow before classification itself lands; an unmatched saved region shows "(missing from SD)" rather than silently reverting. **NTP server is now configurable** (§3.4/§5): a dropdown of common choices (pool.ntp.org default, regional pools, Google/Cloudflare/NIST) plus a Custom text field, applied via a new `wifi_restart_sntp()` that deinits/reinits `esp_netif_sntp` with the new hostname on a live connection — no reboot required, unlike the WiFi-tab's IP settings. All settings apply immediately per FSD §5's "no reflashing" rule; `POST /api/settings` clamps every numeric field server-side and rejects region/ntp values containing path or quote characters. Verified live: defaults round-tripped through `GET /api/settings` after a fresh flash, a full save (`nestbox`/UTC0/custom region+quality) read back correctly and survived a reboot, then reverted to defaults. Firmware 0.8.0.
- v1.8 — **Stats tab and WiFi tab implemented (firmware 0.7.0); §3.4 statistics and §4.5 static IP now live.** New `stats.c`: aggregates the monthly visit-log CSVs into daily/species/hourly buckets on request (newest 12 log files, 62 day / 24 species caps; a user-corrected label wins over the logged species; pre-SNTP "unsynced" rows count toward species totals but not the date/hour buckets, honouring §3.4's no-~1970-dates rule). New endpoints `GET /api/stats/daily|species|hourly` (§6); the ~2.6 kB aggregate lives on the heap, not an httpd stack, and the list responses stream chunked. Stats tab renders bar rows (visits/day, last 30), a 24-column hour-of-day histogram and a species table (visits, first/last seen) — pure inline JS, no CDN (§3.4). WiFi tab replaces the dashboard's link to the portal page: in-tab network scan + credential save, and the §4.5 IP configuration — DHCP (default) or static IPv4 with server-side validation (`400 invalid IPv4 address` — a syntactically bad static IP would associate but be unreachable, indistinguishable from a hang), persisted in NVS, applied to the STA netif before `esp_wifi_start()`; an invalid *saved* config degrades to DHCP rather than bricking the unit, and the §4.6 boot-button reset now erases IP config too — the documented recovery from an unreachable static address. `max_uri_handlers` 16 → 24 for the five new routes. Verified live on the reference unit: stats cross-check exactly against the card's 8 logged visits (6 in the 21 h bucket of 07-06 + 1 in the 6 h bucket of 07-07 + 1 unsynced species-only row); `999.1.2.3` rejected with 400; full static-IP round-trip — saved the current DHCP address as static, device rebooted and came back reachable with `static:true` and matching live values, then reverted to DHCP and confirmed. Firmware 0.7.0.
- v1.7 — **Gallery tab implemented (firmware 0.6.0), first slice of the §5 tabbed UI.** Dashboard rebuilt as the real tab bar (Live | Gallery | WiFi-link; remaining tabs land with their subsystems). Live tab: stream + status ticker + Snapshot-to-SD button; **leaving the Live tab drops the stream `img src` to free one of the two §3.3 stream slots**. Gallery tab: day selector built from new `GET /api/days` (day-folders + file counts), thumbnail grid from new `GET /api/events?date=` (files + sizes; lazy-loaded, newest first), click-through to full size, per-capture delete via new `DELETE /captures/...` (traversal-guarded, confirm dialog). Both listing endpoints stream chunked JSON — no fixed buffer to outgrow on a full card. Favorites and species-label correction (§3.4) deferred to the species-ID milestone, which brings the event metadata they attach to; `GET /api/events` currently lists files rather than the FSD §6 event objects — the API shape converges when events gain IDs/metadata with §3.2. Verified live: day list and file list match the card's real contents (incl. a 4-frame motion burst from §3.1's follow-up logic), throwaway snapshot created → deleted → confirmed 404. Firmware 0.6.0.
- v1.6 — **Motion-triggered capture implemented (firmware 0.5.0), §3.1 live.** `motion.c`: detection frames are the sensor's JPEGs decoded at 1/8 scale (SVGA → 100×75) via `esp_jpeg`, reduced to grayscale (green channel — differencing needs a stable transform, not colorimetry); per-pixel delta > 25 counts as changed; motion when the changed area exceeds a sensitivity-derived threshold (sensitivity 0→11 %, 50→6 %, 100→1 % of frame); rolling background is a 7/8-EMA updated **only on no-motion frames** so a stationary bird isn't absorbed mid-visit, and re-seeded after each event's cool-down. Event pipeline in the motion task: first frame saved immediately, then up to `capture_count`−1 follow-ups at `capture_interval_ms`, **stopping early when motion ends between frames**; then a visit-log CSV row (`storage_append_visit_log()`, monthly `/sd/log/visits-YYYY-MM.csv` with header on creation, species "unclassified" until §3.2) and the `cooldown_s` cool-down. §7's single-writer requirement satisfied by the motion task + write mutex (API snapshots share the mutex) — no separate queue task needed at this producer count. `/api/status` gains `motion`/`events`/`lastEvent`; dashboard ticker shows them with a link to the latest capture (2 s refresh). Verified live: a hand-wave in front of the reference unit triggered event #1 within seconds of boot, one frame saved (early-stop correct — the hand had left), status fields updated, frame fetched back over LAN. Firmware 0.5.0.
- v1.5 — **microSD storage implemented (firmware 0.4.0), §7 partially live.** `storage.c`: SDMMC 1-bit mount at `/sd` (CLK 39 / CMD 38 / D0 40 — the assumed Freenove layout from v1.2, now **verified live**: 64 GB card mounts and round-trips data on the reference unit); never auto-formats a user's card; creates `/captures` `/log` `/model`; missing/unmountable card degrades to a clear no-SD state (§7). `storage_save_jpeg()` writes `/captures/YYYY-MM-DD/HHMMSS.jpg` from the SNTP clock, uptime-named files under `no-date/` before first sync, same-second collisions get a letter suffix. Writes serialized by mutex for now — the §7 queue-based writer task is deferred to the motion-capture pipeline, its first asynchronous producer. New endpoints (§6): `POST /api/capture` (manual snapshot → SD, returns path+size), `GET /captures/...` (static JPEG serving from SD; wildcard route, path-traversal guarded), and `/api/status` gains `sdPresent`/`sdTotalMB`/`sdFreeMB`. Verified end-to-end over LAN: API snapshot saved with correct timestamp path and fetched back byte-identical. Remaining from §7: retention pruning (arrives with capture pipeline), visit-log CSVs (arrives with §3.4).
- v1.4 — **Live MJPEG streaming implemented (firmware 0.3.0), §3.3 now live.** `camera.c`'s temporary auto-probe replaced by real init against the `board_config.h` pin map: JPEG at SVGA 800×600 (the §3.3 stream default), 2 frame buffers in PSRAM, `CAMERA_GRAB_LATEST` so a slow client can't back the sensor up; a missing camera degrades to a clear "no camera" state (503 on `/stream`, `camera_available()` for the UI) instead of aborting boot. New `GET /stream`: `multipart/x-mixed-replace` MJPEG. **Design note: ESP-IDF's httpd is single-threaded, so an endless synchronous stream handler would block every other request; each stream client runs in its own FreeRTOS task via `httpd_req_async_handler_begin()` on port 80** — considered and rejected the esp32-camera example's second-httpd-on-:81 approach, which would break the FSD §6 single-origin API. Max 2 concurrent clients (503 beyond, §3.3). Root dashboard now embeds the live stream with a 5 s status ticker. Verified end-to-end over the LAN on the reference unit: ~17 JPEG frames pulled in 4 s by an external client, sample frame decoded correctly, `/api/status` responsive while streaming. Firmware 0.3.0.
- v1.3 — **WiFi provisioning implemented (firmware 0.2.0), §4 now live.** `wifi.c` + `web_server.c` port the RemoteStart flow: first-boot SoftAP portal (`BirdBox-Config`/`birdbox1234`, setup page at 192.168.4.1 with network scan, URL-decoded credential save to NVS, reboot); boot connect 15 s/5 retries; **on connect-failure with stored credentials the portal opens in APSTA mode and retries the stored network every 60 s in the background, rebooting into normal mode if it succeeds** (§4.4 — implemented as specified, note this *differs* from RemoteStart, which stops STA retries entirely while its portal is open); once ever-connected, in-service disconnects retry indefinitely and never reopen the portal; `WIFI_PS_NONE` + max TX power; boot button (GPIO0) held ≥ 5 s at power-up erases credentials; SNTP (`pool.ntp.org`) + settings timezone started on connect. Web server carries the RemoteStart httpd lessons (LRU purge, handler headroom, loud registration failures) and serves: portal/setup page, placeholder dashboard, `GET /api/scan`, `GET /api/status` (minimal), `POST /api/reboot`, `POST /wifi-save`. Verified live on the reference unit: portal AP + DHCP up, 6 routes registered, camera probe unaffected. Static IP (§4.5) and the full WiFi tab remain for the web-UI milestone.
- v1.2 — **Reference hardware identified and pinned (§2.1).** The project's physical unit (sold as "ESP32-CAM OV2640" on eBay) is actually a generic **ESP32-S3-CAM, N16R8** (ESP32-S3 QFN56 rev v0.2, 16 MB flash, 8 MB embedded octal PSRAM) — identified via esptool chip probe plus a temporary firmware auto-probe in `camera.c` that tried known S3 camera pin maps until SCCB answered: the ESP32-S3-EYE/Freenove map matched (XCLK 15, SIOD/SIOC 4/5, OV2640 at 0x30, PID 0x26), confirmed with a live 800×600 JPEG test frame. `board_config.h` gains a `BOARD_ESP32S3_CAM_GENERIC` block (now the default) with the verified camera pins; LED and SD pins marked TODO-verify. S3 build bumped from assumed 8 MB to real 16 MB flash: OTA slots grown to 2×4 MB and a new 7.9 MB FAT `model` data partition added as the on-flash fallback location for the species model (§3.2). The camera probe stays in `camera.c` until the real capture pipeline replaces it — it doubles as a bring-up diagnostic for other boards.
- v1.1 — Project scaffolded (firmware 0.1.0): ESP-IDF project at repo root with per-target `sdkconfig.defaults.esp32s3`/`.esp32` and partition tables (8 MB S3 with 2×3 MB OTA slots; 4 MB ESP32-CAM with the RemoteStart layout), one stub module per subsystem in `main/` (`wifi` `web_server` `camera` `motion` `capture` `classify` `settings` `storage`, each marked `TODO(FSD §…)`), `esp32-camera` as a managed component, `board_config.h` with pin maps for Seeed XIAO ESP32S3 Sense (default) and AI-Thinker ESP32-CAM, CI workflow (build both targets on every push; `v*` tag → GitHub Release with both `.bin`s), README, MIT LICENSE, docs/ placeholder. No functional requirements changed.
- v1.0 — Initial specification.

---

## 1. System Overview

BirdBox is an open-source, WiFi-connected nest box / bird feeder camera built on an **ESP32-S3 camera module** running **ESP-IDF** (native Espressif SDK — no Arduino framework). It watches a nest box or feeder, detects bird activity, captures photos/short clips, identifies the species with an on-device AI model, and serves everything through its own built-in web UI: live view, capture gallery, and visit statistics.

The device is fully self-contained: no cloud service, no companion app, and no external server is required. Everything — capture storage, species identification, the web dashboard — runs on the device itself, with captures stored on a microSD card. It is designed for public/open-source users: any hobbyist should be able to buy the supported hardware, flash a release binary, join it to their WiFi via the first-boot portal, and mount it on a nest box.

```
 Bird activity ──► Camera + PIR ──► Motion detect ──► Capture (photo/clip) ──► microSD
                                                          │
                                                    Species ID (on-device AI)
                                                          │
 Browser ◄──── Web UI: Live stream | Gallery | Statistics | WiFi | OTA
```

**Development framework:** ESP-IDF v6.x (native Espressif SDK). Project structure, build system (`idf.py`/CMake), WiFi provisioning, web server, OTA and CI conventions follow the proven patterns from the RemoteStart project (`D:\SteinsRootMappe\Claude\RemoteStart`).

---

## 2. Hardware

### 2.1 Target board

| Board | MCU | Camera | PSRAM | Role |
|---|---|---|---|---|
| **Generic "ESP32-S3-CAM"** (N16R8: 16 MB flash, 8 MB octal PSRAM; ESP32-S3-EYE/Freenove-compatible camera pin map) | ESP32-S3 (dual-core LX7, vector instructions) | OV2640 | 8 MB (required) | **Primary target & the project's reference unit** (identified 2026-07-06 by SCCB probe: sensor PID 0x26, camera pin map verified live). Other S3 camera boards (XIAO ESP32S3 Sense, Freenove, S3-EYE) supported via `board_config.h`. |
| AI-Thinker ESP32-CAM | ESP32 classic | OV2640 | 4 MB | Secondary/constrained target: capture, streaming and gallery work; on-device species ID reduced or disabled (§3.2) |

Exact pin maps are board-specific and defined per-board in a `board_config.h`; the FSD does not fix GPIO numbers. Required peripherals:

| Peripheral | Interface | Purpose |
|---|---|---|
| Camera sensor (OV2640 or better) | DVP via `esp32-camera` component | Photo/clip capture, live stream, motion detection |
| microSD card | SDMMC (1-bit or 4-bit) or SPI, FAT32 | Capture storage, visit log database |
| PIR motion sensor (optional) | GPIO input | Low-power wake / first-stage motion trigger |
| IR LED illuminator (optional) | GPIO output | Inside-nest-box viewing in darkness (requires IR-filter-less sensor variant) |
| Status LED | GPIO output | Boot/WiFi/portal state indication (blink patterns, RemoteStart-style) |

### 2.2 Placement variants

The same firmware supports two mounting scenarios, selected in settings (§5):

- **Nest box mode** — camera inside a closed box, close focus, IR illumination, activity is relatively rare and long-duration (nesting).
- **Feeder mode** — camera pointed at a feeder/perch in daylight, activity is frequent and short (visits of seconds), species variety is high.

The mode primarily tunes motion-detection sensitivity, capture cadence and statistics presentation; the feature set is identical.

---

## 3. Functional Requirements

### 3.1 Motion-triggered capture

- Continuous camera-based motion detection: low-resolution grayscale frame differencing against a rolling background, with configurable sensitivity threshold and a minimum-changed-area filter to reject leaves/light changes.
- Optional PIR pre-trigger: when a PIR sensor is fitted, it acts as the first stage (and, in a future battery variant, a deep-sleep wake source); camera-diff confirms before capture.
- On trigger, the device captures a **visit event**:
  - 1 full-resolution JPEG immediately, plus up to N (default 5, configurable) follow-up frames at a configurable interval while motion persists,
  - optionally a short MJPEG clip (default off — SD write bandwidth permitting).
- Debounce/cool-down (default 10 s, configurable) so one continuous visit produces one event, not dozens.
- Each event is written to microSD as `/captures/YYYY-MM-DD/HHMMSS_<seq>.jpg` plus one row in the visit log (§3.4).
- Retention: configurable cap on SD usage (default 80 %); oldest day-folders are pruned first. Events whose species ID is flagged "favorite" by the user are exempt from pruning.

### 3.2 AI species identification

- Every visit event's best frame is classified on-device by a quantized bird-species model (esp-dl / TFLite-Micro int8, running on the S3's vector unit).
- Model: a compact classifier (MobileNet-class, ~1–4 MB in flash or loaded from SD) trained on common garden/feeder species, with a **regional species list** — v1 ships a Northern-Europe model; the model file lives on the SD card so users can swap in other regions without reflashing.
- Output per event: top-3 species with confidence scores; results below a configurable confidence threshold (default 60 %) are labeled **"Unidentified bird"** rather than guessed.
- A "no bird" class guards against false triggers (squirrels, wind); such events are kept but flagged, and can be auto-pruned.
- The user can correct a wrong label from the gallery; corrections are stored with the event (and can optionally be exported to help improve the shared model — see §9).
- **ESP32-classic fallback:** on non-S3 hardware inference is disabled or limited to bird/no-bird; the UI shows captures as "Unclassified". All other features work identically.
- Classification is asynchronous: capture and storage are never delayed waiting for inference; the label attaches to the event when ready (typically < 5 s later).

### 3.3 Live streaming / remote viewing

- MJPEG live stream over HTTP (`/stream`), viewable in any browser on the LAN — embedded in the web UI's Live tab and usable as a direct URL (e.g. as a camera source in Home Assistant/VLC).
- Configurable stream resolution/quality, independent of capture resolution; default 800×600 to keep capture quality unaffected.
- At most 2 concurrent stream clients (memory bound); further clients get HTTP 503 with a clear message.
- Streaming and motion detection coexist: while a client streams, motion detection keeps running (detection uses its own low-res frames).
- Remote (outside-LAN) viewing is explicitly **out of scope for the device itself** — no cloud relay, no port-forwarding automation. The documented path is the user's own VPN (WireGuard/Tailscale) to their LAN. This is a deliberate security decision for an open-source device.

### 3.4 History & statistics

- Every visit event is appended to a visit log on SD (`/log/visits.csv`, append-only, one file per month): timestamp, species, confidence, frame count, file paths, user correction.
- **Gallery tab:** browse captures by day, filter by species, view full-size frames, mark favorites (pruning-exempt), correct species labels, delete events.
- **Statistics tab:**
  - visits per day/week/month (bar chart),
  - species leaderboard (distinct species, visit counts, first/last seen),
  - activity-by-hour-of-day profile,
  - "new species" flag when a species appears for the first time.
- Charts are rendered client-side in the web UI from JSON APIs (§6); the device only serves data, keeping firmware HTML small (RemoteStart pattern: single-file embedded UI, no external CDN dependencies — the UI must work with no internet access).
- Time base: SNTP, server configurable in Settings (default `pool.ntp.org`), timezone configurable (default `Europe/Oslo`, auto-DST). Until first sync, events get a placeholder timestamp and are re-based when the clock syncs — statistics never show ~1970 dates (lesson from RemoteStart §6.1).

---

## 4. WiFi Configuration (First Startup)

Identical in design to the RemoteStart project's provisioning (its FSD §4 / `main.c` WiFi section), reimplemented in this codebase:

1. On first power-up — or any boot where NVS holds no WiFi credentials — the unit opens a SoftAP config portal:

   | AP Name | Password |
   |---|---|
   | `BirdBox-Config` | `birdbox1234` |

2. The portal page (served at the AP's root, `192.168.4.1`) offers a **WiFi network scan** — the user picks their SSID from the scanned list (or types a hidden one) and enters the password.
3. Credentials are URL-decoded and saved to NVS (`ssid`/`pass` in the `birdbox` namespace); the unit reboots and connects as a STA.
4. Boot-time connect: 15 s timeout / 5 retries. On failure **with stored credentials**, the unit reopens the portal *in APSTA mode* while continuing to retry in the background (a nest box may be at the edge of range; the portal must not permanently strand a temporarily-offline unit). Once connected the first time, in-service disconnects retry indefinitely and never reopen the portal (RemoteStart v1.25 lesson).
5. A **WiFi tab** in the normal web UI (always available, not just first boot) allows changing SSID/password and choosing **DHCP or static IP** (IP/mask/gateway/DNS, server-side IPv4 validation, invalid saved config falls back to DHCP) — the RemoteStart v1.37/v1.38 design.
6. Credentials reset: hold the boot button ≥ 5 s at power-up → NVS WiFi namespace erased → portal reopens.
7. `WIFI_PS_NONE` and max TX power from day one (RemoteStart v1.32 lesson — modem-sleep latency ruins HTTP streaming), and `httpd` with `lru_purge_enable = true` and handler-count headroom (v1.35/v1.27 lessons).

---

## 5. Web UI

Single-page UI embedded in firmware (no filesystem-served assets, no CDN), tab bar following the RemoteStart standard:

**Live | Gallery | Stats | Settings | Debug | WiFi | OTA Update**

- **Live** — MJPEG stream, snapshot button, current motion-detection state indicator.
- **Gallery** — §3.4 browsing/labeling.
- **Stats** — §3.4 charts.
- **Settings** — placement mode (nest box/feeder), motion sensitivity, capture count/interval, cool-down, confidence threshold, retention cap, stream quality, species-model region (§3.2), timezone, NTP server, IR LED mode (off/auto).
- **Debug** — System card (free heap + low-water mark with age, uptime, WiFi reconnect count + last-reconnect age), WiFi Link card (RSSI/channel/own MAC), SD card status (size/free/health), camera sensor status, last-inference timing.
- **WiFi** — §4 step 5, plus a Reboot Now button.
- **OTA Update** — §8.

All settings persist in NVS and apply without reflashing; settings that require a restart (camera resolution) say so and offer the reboot.

---

## 6. REST API

All UI data flows through JSON endpoints, so the device is scriptable/integrable (Home Assistant etc.) without scraping:

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status` | GET | Current state: motion, last event, species, SD/heap/WiFi summary |
| `/api/sysinfo` | GET | Debug-card data (heap, heapMin+age, uptime, reconnects) |
| `/api/events?date=&species=&page=` | GET | Paged visit log |
| `/api/events/<id>` | PATCH / DELETE | Correct species label, favorite, delete |
| `/api/stats/daily`, `/api/stats/species`, `/api/stats/hourly` | GET | Chart data |
| `/api/capture` | POST | Manual snapshot now |
| `/api/settings` | GET / POST | Read/write settings |
| `/api/ipconfig`, `/api/ipconfig/save` | GET / POST | DHCP/static IP (RemoteStart design) |
| `/api/reboot` | POST | Reboot |
| `/stream` | GET | MJPEG live stream |
| `/captures/...` | GET | Static serving of stored JPEGs from SD |

No authentication in v1 (LAN-only device, same posture as RemoteStart); an optional basic-auth password is a v2 candidate and noted in §11.

---

## 7. Storage

- **microSD (FAT32)**: captures (`/captures/YYYY-MM-DD/`), visit logs (`/log/visits-YYYY-MM.csv`), optional model file (`/model/`). The device boots and runs without an SD card — live view still works; capture/history features show a clear "no SD card" state instead of failing silently.
- **NVS**: WiFi credentials, IP config, all §5 settings, favorite-species list.
- SD writes are sequenced through a single writer task (camera capture, log append and HTTP static serving must not interleave mid-file).

---

## 8. OTA Updates

- Web-based OTA (OTA Update tab): upload a `.bin` from the browser, RemoteStart-style, with bounded `httpd_req_recv()` retries (v1.5 lesson) and dual OTA partitions with rollback on boot failure.
- Release binaries are produced by CI (§9) so open-source users never need an ESP-IDF toolchain to stay current: download `birdbox-vX.Y.Z.bin` from GitHub Releases → OTA tab → upload.

---

## 9. GitHub, CI & Open-Source Deliverables

- **Repository:** GitHub, public when ready for release. Layout:
  ```
  BirdBox/
  ├── FSD_BirdBox.md          # this document — updated with a changelog entry per change (RemoteStart practice)
  ├── README.md               # user-facing: hardware list, wiring, flash & first-boot walkthrough
  ├── LICENSE                 # MIT
  ├── main/                   # ESP-IDF app (C)
  ├── components/             # esp32-camera, esp-dl model runner
  ├── docs/                   # enclosure/mounting notes, model-swap guide
  └── .github/workflows/      # CI
  ```
- **CI (GitHub Actions):** every push builds with the pinned ESP-IDF version for both targets (esp32s3 primary, esp32 fallback); a `v*` tag creates a GitHub Release with the built `.bin` files and auto-generated per-target version lines (RemoteStart v1.16 pattern).
- **Versioning:** `FIRMWARE_VERSION` in `version.h`, semver; FSD changelog is the change record.
- **Open-source posture:** issues/PRs welcome; docs must be good enough that a stranger with the listed hardware succeeds without asking. Community label corrections (§3.2) may be pooled via voluntary GitHub issue uploads to retrain the regional models — no telemetry, ever, and nothing leaves the device automatically.

---

## 10. Development Environment

- ESP-IDF v6.x on Windows (`idf.py build/flash/monitor`), same toolchain installation as RemoteStart/esp32_clock.
- `idf.py set-target esp32s3` for the primary board.
- `sdkconfig.defaults` per target committed to the repo (PSRAM enabled, camera, FATFS long filenames, httpd tuning per §4.7).

---

## 11. Non-Goals (v1) & Future Candidates

**Out of scope for v1:**
- Cloud services, accounts, or any off-LAN access built into the device (§3.3).
- Audio capture / birdsong identification.
- Battery/solar power management and deep-sleep operation (PIR wake groundwork is laid, §3.1).
- Video with audio; H.264 encoding.
- Web UI authentication.

**Likely v2 candidates:** basic-auth for the web UI, birdsong ID (I2S microphone), battery/deep-sleep mode, MQTT publishing for Home Assistant, multi-camera aggregation page.

---

## 12. Acceptance Criteria (v1)

1. Fresh flash → `BirdBox-Config` portal → scanned SSID selected → device on home WiFi in under 3 minutes, no toolchain needed.
2. A bird landing at the feeder produces exactly one visit event with ≥ 1 sharp full-res JPEG on SD and a visit-log row.
3. A common regional species in good light is correctly top-1 identified ≥ 70 % of the time; low-confidence events say "Unidentified bird", never a confident wrong guess above threshold.
4. Live stream viewable in a browser while captures continue to be recorded.
5. Stats tab correctly aggregates a week of real events by day, species and hour.
6. OTA from GitHub Release binary succeeds and survives a mid-upload abort without wedging the web server.
7. Device runs ≥ 7 days unattended with no reboot, no heap-low-water regression, and survives router reboots (indefinite reconnect).
