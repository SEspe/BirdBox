# Session Notes — BirdBox (updated 2026-07-14)

Rolling notes for the "BirdBox" work. Firmware is now **0.68.1**, FSD **v1.89**.
Everything committed and pushed to `origin/master`; **0.68.1 is OTA-flashed and
live** on the reference unit at **192.168.1.111**. GitHub **Releases v0.64.0
through v0.68.1** are all published (CI-built binaries attached).

## Current device state (verified live)
- **version 0.68.1**, clock `ntp`, SD present.
- **Free internal DRAM ~181 KB** (largest block ~102 KB), **free PSRAM ~702 KB**
  (largest block ~704 KB) — steady even under camera+classify load (both run in
  PSRAM). These are the new `/api/sysinfo` fields (v1.87).
- **Resolution: HD 1280×720 (res=3)**, JPEG quality 8, `detect_zoom = 0`.
- **motion_sensitivity 76**, boot quarantine 30 s, cooldown 3 s, interval 1500, count 5.
- **`conf` (classify threshold) = 60** — very high (see gotchas). User's call.
- **Device address: 192.168.1.111** (confirmed live). CLAUDE.md's `192.168.10.236`
  is **stale** — always confirm with `GET /api/status`.

## What we did this session (0.64.0 → 0.68.1)

### 6. Fix Gallery multiselect delete truncating large selections — 0.68.1, FSD v1.89 (4aeb7c7), Released
- **Reported:** selecting all 285 unclassified frames on a day and hitting
  **Delete selected** didn't delete them all.
- **Root cause:** `h_captures_delete_batch` (`POST /api/captures/delete`) read the
  body with a **single** `httpd_req_recv()` — the one batch handler that *didn't*
  loop. The multiselect list is one `files=a.jpg,b.jpg,...` body (~8 KB for 285
  names) that TCP splits across segments; the single recv got only the first
  ~1.4 KB, so ~50 files deleted and the rest were silently dropped (JS ignored the
  returned `deleted` count).
- **Fix:** loop `httpd_req_recv` until the whole body is read (up to the 16 KB cap
  ≈ 560 files), matching the sibling handlers (`/api/relabel`, `/api/confirm`,
  `/ota/from-url`); `gDelSel()` now compares `deleted` vs requested and alerts
  *"Deleted N of M — reload and delete the rest"* so a truncation can never pass
  silently again. Whole-day **Delete all**/**Wipe day** (`all=1`) were never
  affected (no file list).
- **Verified:** user re-ran the 285-frame delete on 2026-07-13 — all removed OK.


### 1. Gallery bulk-label ops + Maintenance tab — 0.64.0, FSD v1.84 (a9eed9a), Released
- **✔ Confirm selected** (loops `/api/confirm`) and **🔖 Set species▾** (inline
  datalist picker, loops `/api/relabel`, seeds copy-last) — selection-scoped bulk
  labelling. Both write the visit-log `corrected` column → feed the retrain export.
- Requests issued **strictly sequentially** (`gSeq`) — the ESP32 httpd has only a
  handful of worker sockets, so never a fan-out. **No new firmware routes** (reuses
  the per-image handlers).
- **New "Maint" tab** — moved the whole-day destructive ops (**Delete all photos**,
  **Wipe day**) out of the Gallery bar (mis-click risk next to labelling) into their
  own tab with a day selector. UI-only.

### 2. Motion false-positive fix — global-illumination compensation — 0.65.0, FSD v1.85 (1a88237), Released
- **Reported:** motion firing on the empty feeder, several *exactly when the live
  view was opened*. Pulled the frames — no bird, just a whole-frame brightness swing
  (auto-exposure re-converge when `/stream` starts; motion runs on the *same single
  sensor stream*).
- **Root cause:** `motion.c` used a fixed absolute per-pixel threshold with no global
  compensation; the dual-EMA + 20-cell cap only stop *gradual*/*uniform* changes, so
  a sudden non-uniform AE swing (bright stump clips, dark shed doesn't) forms a
  bird-sized cluster and triggers.
- **Fix:** subtract each background's frame-mean brightness delta before thresholding
  (uniform step cancels; a local bird still stands out — it shifts the mean only
  ~1-3 levels). Guard: a frame whose mean shift exceeds `GLOBAL_STEP_THR = 12` can't
  trigger; EMA re-seeds so detection recovers in ~1 s. Zone/sensitivity untouched.
- **Verified:** reproduction (opening `/stream` repeatedly) → 0 new captures.

### 3. Progress bar for bulk-label ops — 0.66.0, FSD v1.86 (cba0090), Released
- Determinate bar below the gallery toolbar ("Confirming – 34 / 120 (28%)" /
  "Setting species – …"), updated per image, auto-hides ~0.5 s after finishing.
  UI-only; no change to the sequential pattern.

### 4. Heap diagnostic: internal DRAM + PSRAM reported separately — 0.67.0, FSD v1.87 (f1fca8b), committed (not released)
- `esp_get_free_heap_size()` lumps both pools; added `heapInt`/`heapIntBig` and
  `heapPsram`/`heapPsramBig` (free + largest contiguous block per pool) to
  `/api/sysinfo` and the Debug tab. Groundwork for the two memory questions below.
- **Measured:** ~180 KB internal DRAM free (100 KB largest block), steady under load;
  ~705 KB PSRAM free. This is what made the OTA-from-URL memory call a clear "yes."

### 5. Flash a GitHub release directly from the OTA tab — 0.68.0, FSD v1.88 (b351fed), Released
- OTA tab lists this repo's releases (all of them, running one marked) and flashes a
  chosen one over the air, with a progress bar. **No PC/cable.**
- **Why device-side download:** the GitHub *API* sends CORS `*` (browser can list),
  but the release *asset* redirects to `release-assets.githubusercontent.com` with
  **no CORS header** (browser can't read the bytes). So: browser fetches the release
  list + POSTs the picked asset URL to **`POST /ota/from-url`**; the device downloads
  +flashes via `esp_https_ota` in a background task; **`GET /ota/from-url`** reports
  `{state,read,total,msg}` for the poll.
- **Security — repo-locked:** rejects any URL not starting with
  `https://github.com/SEspe/BirdBox/releases/download/`. Reuses dual-partition rollback.
- **Build:** adds `esp_https_ota`/`esp_http_client`/`mbedtls` to `main`, enables
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` + `DYNAMIC_BUFFER` in `sdkconfig.defaults`
  (clean rebuild required; +140 KB flash → 65% partition free).
- **Verified end-to-end:** device self-downloaded v0.68.0 from GitHub (1,476,624 B,
  100%, ~21 s) through the redirect + cert-bundle validation, rebooted healthy on
  0.68.0. Repo-lock rejected `evil.example.com` and a wrong-repo URL. Internal DRAM
  unchanged post-flash (~181 KB) — cert bundle is flash, not heap.

## Memory evaluation (settled this session — see v1.87/v1.88)
- **Camera + classifier run entirely in PSRAM**, so internal DRAM sits at ~180 KB
  free (100 KB largest block) and barely moves under load.
- **TLS/OTA-from-URL:** peak ~40-60 KB internal → ~120 KB headroom. Comfortable.
  `DYNAMIC_BUFFER` shrinks the peak further. TLS buffers left in internal DRAM
  (proven fine); PSRAM-routing was considered but unnecessary.
- **New model (tomorrow):** the TFLM arena is a single big **PSRAM** alloc; the
  ceiling is `heapPsramBig` ≈ **704 KB** contiguous above the current model. Load the
  new model and read `heapPsram`/`heapPsramBig` before/after — the drop *is* the
  arena; a failed alloc shows immediately in the Debug tab.

## Key decisions
- Bulk-label ops **sequential, never fanned out** (tiny httpd socket pool); progress
  bar makes the wait tolerable. Reuse existing endpoints (no route-table growth).
- Destructive day-ops isolated in the Maintenance tab.
- Global-illumination compensation is an **algorithmic robustness fix**, separate
  from zone/sensitivity (operator-owned, untouched). A zone change wouldn't fix an
  AE swing — it hits every cell. `GLOBAL_STEP_THR=12` set conservatively to protect
  bird sensitivity.
- OTA-from-URL is **repo-locked** (can only flash SEspe/BirdBox signed releases) and
  reuses rollback — a bad image reverts on next boot.
- Route table now 43/48 — still headroom, but watch it.

## Gotchas (still live)
- **`conf` threshold is 60** — very high; real birds under 60% log as *unclassified*.
  Likely an NVS-wipe reset from the tuned ~17. Restoring it is the user's call.
- **Detection zone is permissive** — bottom row (foreground grass) + right-side tree
  line are active wind-driven false-positive sources. Tightening helps *wind*
  triggers (not AE ones). **User owns the zone** — explain, don't apply uninvited.
- **Motion shares the single sensor stream** — opening live view perturbs AE (that's
  what 0.65.0 fixes). Watch for any residual localized-clip false positives; if one
  sneaks through, lower `GLOBAL_STEP_THR` from 12 (with live evidence).
- **Whole UI is one inline `<script>`** — one JS syntax error kills all handlers; the
  C build can't catch it. Grep the served page + bracket/ternary balance pass.
- **sdkconfig.defaults changes need `sdkconfig` deleted** to regenerate (done for
  0.68.0). **Build must run from the repo root** (deletes any stray `main/build`).
- **Boot quarantine (30 s)** and **still-bird-absorbed-into-background** both look
  like dead detection but aren't.

## What's left to do
- [ ] **New/enhanced model (tomorrow's plan)** — user will clear classified images
  then try a new model. Use the new PSRAM/`heapPsramBig` readout to confirm the arena
  fits; the OTA-from-URL feature lets firmware roll fwd/back easily while iterating.
- [ ] **Watch motion false-positive rate** — lower `GLOBAL_STEP_THR` only with live
  evidence a localized clip-swing still triggers.
- [ ] **(Optional) Tighten detection zone** to the stump top to cut wind/grass false
  positives — user's call (paint in Live → zone editor, or ask me to push a mask).
- [ ] **Consider restoring `conf` 60 → ~17** (tuned value) if the high threshold was
  an accidental wipe reset.
- [ ] **Confirm bulk-op + OTA progress bars** fill smoothly with real use in the browser.
- [ ] **v0.4 ROI-crop retrain** — the main open track (ROI logged since 0.60.0; the
  new bulk-label ops speed up seeding the dataset). Data collection is the bottleneck.
- [ ] `bugreport.txt` is the running bug tracker (untracked); bug 1 fixed in 0.63.0.

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`; CLAUDE.md's `.10.236` is
  stale). OTA (manual): `curl -s -X POST -H "Content-Type: application/octet-stream"
  --data-binary @build/BirdBox.bin http://192.168.1.111/ota/upload`. OTA (from GitHub):
  OTA tab → pick release → Download & flash (or `POST /ota/from-url` with the asset URL).
- Build (PowerShell, from repo root): idf_tools.py env-export then `idf.py build`
  (export.ps1 broken — see CLAUDE.md). sdkconfig.defaults change → delete `sdkconfig` first.
- Release: push tag `vX.Y.Z` → `release.yml` CI builds + publishes with the esp32s3 bin.
- Latest commits: `4aeb7c7` (0.68.1 delete recv-loop fix), `b351fed` (0.68.0
  OTA-from-URL), `f1fca8b` (0.67.0 heap split), `cba0090` (0.66.0 progress bar),
  `1a88237` (0.65.0 illum comp), `a9eed9a` (0.64.0 bulk ops + Maint tab).
