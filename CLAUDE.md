# CLAUDE.md

Guidance for AI agents working in this repo. For *what the product does*, read
`README.md`; for the authoritative requirements + change history, read
`FSD_BirdBox.md`. This file is about *how to build, verify, and safely change
the code*.

## What this is

BirdBox — an ESP32-S3 WiFi bird-box/feeder camera built on **ESP-IDF v6.x**
(no Arduino, no cloud). Motion-triggered capture to microSD, on-device TFLite-
Micro species ID, and a seven-tab LAN web UI (Live, Gallery, Stats, Settings,
Debug, WiFi, OTA) served from the device itself. C for the firmware, one C++
file for the classifier, PowerShell + Python for the off-device retrain tools.

## The release contract (do this for every functional change)

A functional change is not done until all three are updated together:

1. **Code.**
2. **`main/version.h`** — bump `FIRMWARE_VERSION` (semver).
3. **`FSD_BirdBox.md`** — add a changelog entry at the top: bump the header
   `**Version:**`, and prepend `- vX.Y — **Title (firmware A.B.C), §section.**
   <what + why + how>`. The FSD changelog is the project's change record; the
   commit history mirrors it.

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

Reference unit is currently at **`192.168.10.236`** (relocated off the old
`192.168.1.111` — always confirm with `GET /api/status` first, which reports
`version`/`ip`/`heap`/`sdPresent`/`clockSrc`).

**OTA (normal path, no cable):** `POST /ota/upload`, raw octet-stream body =
`build/BirdBox.bin`. Dual OTA partitions with automatic rollback if the new
image fails to boot. Use `curl --data-binary` (via the Bash tool) — PowerShell
`Invoke-WebRequest -InFile` hangs on large raw bodies:

```sh
curl -s -X POST -H "Content-Type: application/octet-stream" \
  --data-binary @build/BirdBox.bin http://192.168.10.236/ota/upload
# 200 "OK" -> device reboots; poll /api/status until version flips.
```

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
2. **Flash** (OTA) and confirm `version` via `/api/status`.
3. **Drive the changed flow live** — hit the relevant `/api/…` endpoint or
   exercise it in the web UI; `POST /api/capture` proves the camera path.
4. **For any web-UI change, grep the *served* page** (`curl http://<ip>/`) for
   the tokens you added — the compiler cannot verify the inline JS (see below).

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

- **Classification never drops work.** Prefer queue/wait/degrade over emitting
  "unclassified". Free heap (incl. PSRAM) is only ~600–700 KB in practice, so
  ~1 MB single allocations never succeed — size buffers accordingly and
  `heap_caps_*` into PSRAM for large arrays (e.g. the gallery label table).

- **On the SD (FATFS), `rename()` across directories can delete the source
  without creating the dest.** Use copy-then-delete for cross-dir moves.

- **Species-ID model is data on the SD card, not baked into firmware.** Modern
  TFLM rejects uint8 models; conversion is per-channel symmetric int8 via
  `tools/convert_model_int8.py`. The classifier input contract (`classify.cpp`)
  expects `zero_point==0`, scale `1/128`, XOR-0x80. `esp_jpeg` decode is
  baseline-JPEG only.

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
- `FSD_BirdBox.md` — requirements + full changelog (read this for any "why").

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
