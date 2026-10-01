# CLAUDE.md

Guidance for AI agents working in this repo. For *what the product does*, read
`README.md`; for the authoritative requirements, read `FSD_BirdBox.md`; for why
they are what they are, read `FSD_BirdBox_CHANGELOG.md`. This file is about
*how to build, verify, and safely change the code*.

## What this is

BirdBox — an ESP32-S3 WiFi bird-box/feeder camera built on **ESP-IDF v6.x**
(no Arduino, no cloud). Motion-triggered capture to microSD, on-device TFLite-
Micro species ID, and an eight-tab LAN web UI (Live, Gallery, Stats, Settings,
Maintenance, Debug, WiFi, OTA) served from the device itself. C for the firmware, one C++
file for the classifier, PowerShell + Python for the off-device retrain tools.

## The release contract (do this for every functional change)

A functional change is not done until all four are updated together:

1. **Code.**
2. **`main/version.h`** — bump `FIRMWARE_VERSION` (semver).
3. **`FSD_BirdBox.md`** — update the spec itself to describe the new behaviour,
   and bump the header `**Version:**`.
4. **`FSD_BirdBox_CHANGELOG.md`** — prepend `- vX.Y — **Title (firmware
   A.B.C), §section.** <what + why + how>`. This is the project's change
   record; the commit history mirrors it.

**The two FSD files have different jobs and the split is the point.**
`FSD_BirdBox.md` is the clean current specification, present tense, no history
— it must read as if the design were always this way. All the "we used to do X,
then Y happened" belongs in the changelog. When a change makes a sentence in
the spec wrong, **rewrite that sentence**; do not append a qualifier explaining
what it used to say.

Commit-message convention (see `git log`): `Short imperative summary (A.B.C,
FSD vX.Y)`. Version commits land directly on `master` (linear history).

## Build

ESP-IDF **v6.0.1** lives at `D:\esp\v6.0.1\esp-idf`; the IDF Python env is
`C:\Users\Stein\.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe`.

**`export.ps1` is broken on this machine** (bare `python` hits the Microsoft
Store alias). Set up the env via `idf_tools.py` and call `idf.py` through the
IDF Python env instead (PowerShell):

```powershell
$py = "C:\Users\Stein\.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe"
$env:IDF_PATH = "D:\esp\v6.0.1\esp-idf"
& $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { $n=$matches[1]; $v=$matches[2].Replace('%PATH%',$env:PATH); Set-Item "env:$n" $v } }
& $py "$env:IDF_PATH\tools\idf.py" build
```

- **Target:** `esp32s3` (primary; `esp32` for the AI-Thinker ESP32-CAM, no
  species ID). Pin maps in `main/board_config.h`.
- **Build times:** 5–10 min cold (run in background), seconds warm (ccache).
- **`-Werror` includes `-Werror=comment`** — never write `/*` or a path like
  `/api/x/*` inside a C comment; it fails the build.
- Changing any `sdkconfig.defaults*` requires deleting the generated
  `sdkconfig` (or re-running `idf.py set-target esp32s3`) to take effect.

## Flash & OTA

Two units, both DHCP and both have moved before — **always confirm with**
`GET /api/status` first (reports `version`/`ip`/`heap`/`sdPresent`/`clockSrc`);
try `http://birdbox.local/`, else sweep the /24.

| | address | role |
|---|---|---|
| test | `192.168.10.205` | OV5640. Where everything is tried first. |
| production | `192.168.10.240` | OV2640 @ HD, outdoors. **Do not touch without being asked.** It was the untouched 0.80.0 control for A/B work until 2026-10-01, when it was brought to 1.1.0 at the operator's request — so there is no longer a stale-firmware control. If you need one, say so before updating anything. |

(Earlier addresses, for grep: `192.168.1.111`, `192.168.10.236`.)

**OTA (normal path, no cable):** `POST /ota/upload`, raw octet-stream body =
`build/BirdBox.bin`. Dual OTA partitions with automatic rollback if the new
image fails to boot. Use `curl --data-binary` (via the Bash tool) — PowerShell
`Invoke-WebRequest -InFile` hangs on large raw bodies:

```sh
curl -s -X POST -H "Content-Type: application/octet-stream" \
  --data-binary @build/BirdBox.bin http://192.168.10.205/ota/upload
# 200 "OK" -> device reboots; poll /api/status until version flips.
```

**Two rules around every OTA, both learned the hard way:**

- ~~Set `haen=0` before `POST /ota/upload`~~ — **no longer needed as of 0.99.0.**
  The panic was `MQTT_EVENT_CONNECTED` calling `publish_discovery()` and
  `publish_state()` inside the esp-mqtt callback, i.e. on esp-mqtt's own 6 kB
  task, with ~2.9 kB of buffers. Fixed by announcing from `ha_task` instead
  (FSD v3.36). Re-verified **5/5 clean OTAs with `haen=1`** (2 on 0.99.2, 3 more
  through 1.1.0), `resetReason` `software` every time, against 6/6 panics
  before. If an OTA ever panics with HA enabled again, suspect a stack, not the
  flash write.
- **After flashing, open `http://<ip>/?v=<n>` — not a plain reload.** The whole
  UI is one page carrying its own script, so a cached copy runs the OLD
  release's JavaScript while `/api/status` truthfully reports the new version: a
  new control silently does nothing and every device-side check passes. The page
  sends `no-cache` and shows a banner on a version mismatch, but **a page cached
  before that banner existed cannot warn you** — so a changed query string,
  which is a guaranteed cache miss, is the only reliable way in. This cost four
  flashed releases once; do not repeat it.

**Serial flash (first time / bricked):** from `build/`, enumerate the port
first (`[System.IO.Ports.SerialPort]::GetPortNames()` — it changes across
replugs), then `& $py -m esptool --chip esp32s3 -p COMx -b 460800
--before default-reset --after hard-reset write-flash "@flash_args"`.

Reading the serial boot log resets the board via RTS (there is no
non-invasive peek on this wiring): open the port, then
`s.setDTR(False); s.setRTS(True); sleep(0.1); s.setRTS(False)` before reading.

## Testing / verification

There is **no first-party unit-test suite** (only vendored `managed_components`
ship tests). Verification is empirical, on real hardware:

1. **Build** — catches all C/C++ errors.
2. **Gate the UI JavaScript BEFORE flashing** — `python tools/check-ui-js.py build/BirdBox.bin` parses the inline script straight out of the built image. A JS syntax error builds and flashes cleanly and then kills EVERY handler on the page. Do not skip this on any `web_server.c` change; it has caught a duplicated function tail that had already reached the device.
3. **Flash** (OTA) and confirm `version` via `/api/status`.
4. **Drive the changed flow live** — hit the relevant `/api/…` endpoint or
   exercise it in the web UI; `POST /api/capture` proves the camera path.
5. **For any web-UI change, grep the *served* page** (`curl http://<ip>/`) for
   the tokens you added — the compiler cannot verify the inline JS (see below).

**Know what each check proves — several here proved less than they appeared to:**

- **Parsing the served page proves SYNTAX, not behaviour.** An esprima pass
  happily accepted `getAttribute('href ')` — one stray space that made a click a
  no-op. For a behavioural change, also assert the exact emitted token and
  exercise the endpoint the click calls.
- **A `curl` test with raw slashes does not exercise the browser path**, which
  sends `encodeURIComponent` output. Pass `%2F` explicitly. Beware Git Bash
  mangling it: `curl --data-urlencode "f=/captures/…"` became
  `C%3a%2fProgram+Files%2fGit%2f…` (MSYS path conversion) — a *false failure*
  that was believed and acted on.
- **`POST /api/capture` timing is NOT a per-frame cost proxy.** HD measures
  ~2.57 s against QSXGA's ~2.6–3.2 s despite a 5x smaller JPEG, because the SD
  write and HTTP dominate. Use `/api/motion` `loopMs` for the real detect cadence.
- **Match the test window to the behaviour.** Half an hour at dusk cannot test a
  daylight behaviour: zero triggers is the expected result either way. Check
  `/api/night` `state`/`dark`/`contrast` before reading silence as a fault.

## Load-bearing code patterns & gotchas

- **The entire web UI is ONE inline `<script>` assembled from C string
  literals in `web_server.c`.** A single JS syntax error kills *every* handler
  (tabs go dead) while the live `<img src=/stream>` keeps working (it's HTML,
  not JS) — a classic tell. The C compiler cannot catch this. After editing UI
  JS: keep ternary chains balanced, and verify by grepping the served page
  and/or a bracket/`?`:`-balance pass on the changed fragments. This has bitten
  real releases (e.g. a misplaced paren in a status-line ternary).

- **CSV parsing uses a hand-rolled field scanner (`gal_next_field` in
  `web_server.c`), NOT `strtok`.** `strtok`/`strtok_r` collapse empty fields
  next to delimiters, which silently shifts columns in the fixed-column visit
  log. Any new fixed-column parse must use an explicit next-field scanner.

- **HTTP route table is capped (`max_uri_handlers`).** Registrations past the
  cap are silently dropped and the endpoint 404s while the handler code looks
  fine. When adding routes, keep headroom (currently 48).

- **Guard every "today"/date comparison against the pre-SNTP ~1970 clock.**
  Right after boot `clockSrc` is not yet `ntp`; a naive date compare
  misfiles/misreads. There is a boot detection quarantine for exactly this.

- **Counters that publish only on success answer the wrong question.** Three
  separate blind spots came from this, all fixed by making the quiet path
  visible — copy the pattern rather than re-learning it:
  - `/api/motion` `cells`/`rej`/`c` are **last-trigger snapshots**, written
    after a trigger fires. They read zero whether the detector sees nothing,
    rejects everything, or is not running at all. For "is detection working?"
    use the live fields: `frames`, `loopMs` (real cadence — ~400 ms at HD vs the
    250 ms the loop asks for), `livePct`, `liveClust`/`liveCells` against `thr`,
    `gstep`, `decErr`.
  - The visit log stored a bare `err` for five distinct iNat failures; it now
    stores `err:<reason>` (`401`, `net`, `429`, `tmo`, …) from
    `inat_last_code()`. Keep the short tag separate from the human-readable
    message — the message is free text for the Debug card and changes wording.
  - A full classify queue dropped an event with only an `ESP_LOGW`; `clsQDrops`
    counts it, and `clsQPeak` exists because a 60 s poll cannot see a burst that
    fills and drains between samples.

- **`httpd_query_key_value()` does NOT percent-decode.** Call `url_decode()` on
  every query parameter, as every handler here does — a handler that forgets it
  works perfectly under hand-typed curl and fails on every real browser request.

- **Resolution and detection share one stream.** Raising resolution slows the
  grab+decode the detect loop depends on, so it samples less often and short
  visits fall between samples; the field of view changes with the aspect ratio
  too. **HD is the locked default** (§5) for exactly this reason. Read
  `/api/motion` `loopMs` after any resolution change instead of guessing.

- **Classification never drops work.** Prefer queue/wait/degrade over emitting
  "unclassified".

- **PSRAM is roomy; internal DRAM is the scarce pool.** Measured on the
  reference unit (0.74.44): `heapPsram` **7.9 MB free of 8 MB**, largest block
  **7.5 MB** — only ~480 KB PSRAM in use at idle. Multi-MB PSRAM allocations
  succeed, so `heap_caps_*` large arrays into PSRAM freely (camera framebuffers,
  gallery label table, the 1.5 MB `CLS_DECODE_MAX` ROI decode, all HTTP/TLS reply
  buffers already do). Internal DRAM is the constraint: ~123 KB free with a
  **32 KB** largest 8-bit block, and that is what mbedTLS handshakes compete for
  (see the cert-bundle fragmentation history, §7/v2.50–v2.64). Check
  `/api/sysinfo` `heapInt`/`heapIntBig8` — not the PSRAM-inclusive `heap` field —
  before adding anything that must live in internal RAM.
  *(Pre-0.74.0 this note said free heap was ~600–700 KB and ~1 MB allocations
  never succeeded. That was true only while the TFLM model + 3 MB arena were
  resident in PSRAM; the iNat-only pivot removed both.)*

- **On the SD (FATFS), `rename()` across directories can delete the source
  without creating the dest.** Use copy-then-delete for cross-dir moves.

- **Classification is ONLINE only (as of 0.74.0 / FSD v2.37).** The on-device
  TFLite-Micro model was removed — no more `esp-tflite-micro`, no `/sd/model`, no
  `convert_model_int8.py` (kept but obsolete). `classify.cpp` is now a thin
  iNat+cloud orchestrator: the event task scores frames via `inat.c` (iNaturalist
  online CV) with ≥2-frame corroboration + a Norway geo-filter + best-of-crop,
  falling back to the cloud tier (`cloud.c`). The relabel-picker vocabulary is
  `target_species.h`, not model labels. The 24 h iNat JWT auto-refreshes from a
  stored `_inaturalist_session` cookie (`inat_refresh_jwt`). `esp_jpeg` decode
  (still used by `classify_crop_jpeg`) is baseline-JPEG only.

## Layout

- `main/` — firmware. One module per subsystem: `wifi`, `web_server` (all HTTP
  + the whole UI), `camera`, `motion`, `capture`, `illum` (IR LED), `classify`
  (C++ TFLM), `settings` (NVS), `storage` (SD/FATFS + visit log), `stats`,
  `species_i18n` (localized names), `main.c` (bringup). `version.h`,
  `board_config.h`.
- `tools/` — `convert_model_int8.py` (model → device-ready int8).
- `training-data/` — off-device Nordic-species retrain pipeline: relabel export
  (`export-labels.ps1`), capture puller (`pull-new-captures.ps1`), `train.py`,
  versioned model artifacts. See its `README.md`. `dataset/` is gitignored.
- `docs/MODEL.md` — model install + Northern-Europe region filter.
- `FSD_BirdBox.md` — the clean current specification. What to build.
- `FSD_BirdBox_CHANGELOG.md` — the development record. Read this for any "why",
  and before reopening a settled question.

## The human-in-the-loop / retrain loop (context for gallery + classify work)

Gallery images carry a **per-image state** (`st` in `/api/events`): 0
unclassified, 1 classified (model), 2 confirmed species, 3 no-bird (model), 4
confirmed-no-bird, 5 other/not-a-bird (`other/` hard negative), 6 unknown/bad-
bird (excluded from training). Humans set ground truth via ✎ relabel / ✓
confirm / 🔍 identify → written to the visit log's `corrected` column →
`GET /api/labels/confirmed` → `export-labels.ps1` → `dataset/<class>/`. The
retrain the whole pipeline feeds is FSD §3.2.1/§3.2.2. **The detection-zone
mask and confidence-threshold tuning are the user's calls — explain options,
don't apply changes to them uninvited.**
