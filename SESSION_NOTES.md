# Session Notes — BirdBox (updated 2026-07-14)

Rolling notes for the "BirdBox" work. Firmware is now **0.66.0**, FSD **v1.86**.
Everything committed and pushed to `origin/master`; **0.66.0 is OTA-flashed and
live** on the reference unit at **192.168.1.111**. GitHub **Releases v0.64.0,
v0.65.0, v0.66.0** are all published (CI-built binaries attached).

## Current device state (verified live)
- **version 0.66.0**, clock `ntp`, SD present, free heap ~855 KB.
- **Resolution: HD 1280×720 (res=3)** — compiled default + only-offered choice.
- **JPEG quality 8**, `detect_zoom = 0` (whole-frame center-crop for ID).
- **motion_sensitivity 76**, boot quarantine `qtn=30 s`, cooldown 3 s, interval 1500, count 5.
- **`conf` (classify threshold) = 60** — very high (see gotchas). User's call.
- User-owned NVS tuning (`detect_zone`, `motion_sensitivity`, `conf`) intact across OTAs (OTA preserves NVS).
- **Device address:** the reference unit answers at **192.168.1.111** (confirmed
  live this session). CLAUDE.md's `192.168.10.236` is **stale** — always confirm
  with `GET /api/status` first.

## What we did this session

### 1. Gallery bulk-label ops + Maintenance tab — 0.64.0, FSD v1.84 (a9eed9a), Released v0.64.0
- **Two selection-scoped bulk actions** added to the Gallery bar:
  **✔ Confirm selected** (accepts each tile's model guess as ground truth — loops
  `/api/confirm`; tiles with nothing to confirm are skipped) and
  **🔖 Set species▾** (inline datalist picker, applies one chosen species to all
  selected — loops `/api/relabel`, seeds `g_lastSp` for the 📋 copy-last path).
  Both write the visit-log `corrected` column → feed the retrain export.
- **Sequential runner `gSeq`** — requests issued strictly one-at-a-time, never a
  fan-out, because the ESP32 httpd has only a handful of worker sockets.
- **No new firmware routes** — reuses the existing per-image handlers (stays
  within the `max_uri_handlers` cap).
- **New Maintenance ("Maint") tab** — moved the whole-day destructive ops
  (**Delete all photos**, **Wipe day (photos+stats)**) out of the Gallery bar
  (where they sat next to the labelling controls and could be mis-clicked) into a
  dedicated tab with its own day selector. Same `POST /api/captures/delete` calls;
  UI-only, client-side pane (no route change).

### 2. Motion false-positive investigation → global-illumination fix — 0.65.0, FSD v1.85 (1a88237), Released v0.65.0
- **Reported live:** motion firing on the empty feeder, several **exactly when the
  live view was opened**. Pulled the flagged frames — no bird, just a whole-frame
  **brightness swing** (one frame was blown-out/over-exposed).
- **Root cause (code-level):** motion runs on the *same single sensor stream* as
  the live view. Opening Live starts `/stream`; the OV2640 re-converges its
  auto-exposure/gain → a sudden brightness step. `motion.c` used a **fixed
  absolute** per-pixel threshold (`PIX_DIFF_THR=25`) with no global-illumination
  compensation. The dual-EMA background + 20-cell cluster cap only stop *gradual*
  drift and *uniform* whole-frame changes; a **sudden, non-uniform** AE swing
  (bright stump clips while dark shed barely moves) forms a bird-sized cluster on
  the bright region that slips past both → false trigger.
- **Fix:** subtract each background's **frame-mean brightness delta** before
  thresholding (`abs((cur-bg) - shift) > THR`) — a uniform step cancels to ~0
  everywhere, a real *local* mover still stands proud (a bird shifts the frame
  mean only ~1–3 levels). Plus a guard: a frame whose mean shift exceeds
  **`GLOBAL_STEP_THR = 12`** gray levels is deemed a lighting event and cannot
  trigger; the quiet-frame EMA roll re-seeds the background so detection recovers
  within ~1 s. Zone mask + sensitivity untouched (operator-owned).
- **Verification:** built/flashed; detection still functions; the reproduction
  (opening `/stream` repeatedly to force the AE re-converge) produced **0 new
  captures over 4 open/close cycles** and the trigger count didn't climb. Short
  window — real proof is the evening false-positive rate.

### 3. Progress bar for the bulk-label ops — 0.66.0, FSD v1.86 (cba0090), Released v0.66.0
- The sequential bulk ops (item 1) take real time on a large selection (a busy day
  can be hundreds of frames) and the only feedback was a tiny `i/N` in the status
  line. Added a determinate **progress bar** below the gallery toolbar: `gSeq`
  shows a filling bar with a `"<label> – done / total (pct%)"` readout
  ("Confirming" / "Setting species"), updated per image, auto-hiding ~0.5 s after
  the batch finishes. UI-only (`.gprog*` CSS + `gProg()` helper); no change to the
  sequential pattern, endpoints, or write path.

## Key decisions
- **Bulk-label ops reuse existing endpoints, issued sequentially** — never fan out
  N parallel fetches at the tiny httpd; the progress bar makes the wait tolerable.
- **Destructive day-ops live in their own Maintenance tab** — separated from the
  labelling controls so Delete-all / Wipe-day can't be hit by accident.
- **Global-illumination compensation is an algorithmic robustness fix**, distinct
  from zone/sensitivity tuning (which stay operator-owned and were left untouched).
  A zone change wouldn't fix an AE swing anyway — it hits every cell.
- **`GLOBAL_STEP_THR = 12` set conservatively** to protect bird sensitivity;
  tighten it (lower) only with live evidence that localized highlight-clip swings
  still sneak through.
- **HD remains the one true resolution**; `detect_zoom` stays OFF until a
  ROI-trained model ships; UXGA off the table (PSRAM). (Unchanged this session.)

## Gotchas (still live)
- **Motion shares the single sensor stream** — opening the live view perturbs
  auto-exposure; that's what the 0.65.0 fix addresses. Watch for residual
  localized-clip false positives.
- **`conf` threshold is 60** — very high. Real birds scoring under 60% log as
  *unclassified* (e.g. the 14%/19% near-misses this session). Doesn't affect motion,
  but suppresses genuine IDs. Likely an NVS-wipe reset from the tuned ~17; restoring
  it is the user's call (backup/restore or Settings).
- **Detection zone is fairly permissive** — bottom row (foreground grass) and the
  right-side tree line are active; both are wind-driven false-positive sources.
  Tightening to just the stump top would help the *wind* triggers (not the AE ones).
  **User owns the zone** — explain, don't apply uninvited.
- **Whole UI is one inline `<script>`** — one JS syntax error kills all handlers;
  the C build can't catch it. Verify by grepping the served page (done for every UI
  change this session) + a bracket/ternary balance pass.
- **Build must run from the repo root**, not `main/` — the build command pins
  `Set-Location` to the root and deletes any stray `main/build`.
- **Partial settings POST is safe** — absent fields keep current; zone only applied
  if 64 chars; strings only if non-empty.
- **Boot quarantine (30 s)** and **still-bird-absorbed-into-background** both look
  like "dead detection" but aren't.

## What's left to do
- [ ] **Watch the motion false-positive rate this evening.** If a localized
  highlight-clip AE swing still triggers (mean shift < 12 but a strong local
  cluster), lower `GLOBAL_STEP_THR` from 12 — but only with live evidence, to avoid
  eroding bird sensitivity. Small fw change.
- [ ] **(Optional) Tighten the detection zone** to the stump top to cut the wind/
  grass false positives — user's call; paint it in Live → zone editor, or ask me to
  push a mask.
- [ ] **Consider restoring `conf` 60 → ~17** (user's tuned value) if the high
  threshold was an accidental wipe reset.
- [ ] **Confirm the bulk-op progress bar** fills smoothly to 100% and clears with a
  real large selection in the browser (server-side only the JS presence/balance was
  verified).
- [ ] **v0.4 ROI-crop retrain** — the main open track. ROI has been logged on every
  event since 0.60.0; collect ROI-tagged HD captures → export → implement `train.py`
  ROI-crop → retrain nordic-v0.4 → flip `detect_zoom` ON in the same deploy step.
- [ ] **Data collection is still the real bottleneck** — more diverse Dompap/
  Lavskrike visits. Use the new bulk Confirm/Set-species to label runs faster.
- [ ] `bugreport.txt` is the running bug tracker (untracked). Bug 1 (gallery
  Select-all filter) fixed in 0.63.0; add new ones there.

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`; CLAUDE.md's `.10.236`
  is stale). OTA: `curl -s -X POST -H "Content-Type: application/octet-stream"
  --data-binary @build/BirdBox.bin http://192.168.1.111/ota/upload`.
- Build (PowerShell, from repo root): idf_tools.py env-export then `idf.py build`
  (export.ps1 is broken on this machine — see CLAUDE.md).
- Release: push tag `vX.Y.Z` → `release.yml` CI builds + publishes "Release vX.Y.Z"
  with the esp32s3 bin.
- Latest commits: `cba0090` (0.66.0 progress bar), `1a88237` (0.65.0 illum
  compensation), `a9eed9a` (0.64.0 bulk ops + Maint tab), `7536759` (0.63.0 notes).
