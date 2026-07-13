# Session Notes — BirdBox (updated 2026-07-13)

Rolling notes for the "BirdBox" work. Firmware is now **0.63.0**, FSD **v1.83**.
Everything committed and pushed to `origin/master`; **0.63.0 is OTA-flashed and
live** on the reference unit at 192.168.1.111. A GitHub **Release v0.62.0** is
published (CI-built binary attached); v0.63.0 not tagged yet.

## Current device state (verified live)
- **version 0.63.0**, clock `ntp`, SD present, free heap ~854 KB.
- **Resolution: HD 1280×720 (res=3)** — live + locked compiled default.
- **JPEG quality 8** (`stream_quality=8`, was 12) — live trial for crisper stills; NOT baked as default.
- `detect_zoom = 0` (whole-frame center-crop for ID — see decisions).
- User-owned NVS tuning intact across all the OTAs: `detect_zone`, `motion_sensitivity` (~73), `confidence_pct`. OTA preserves NVS.

## What we did this session

### 1. Parameter-defaults cleanup + Settings backup/restore — 0.61.0, FSD v1.80 (b01f750)
- Retuned compiled defaults so an NVS wipe lands on sane values: `resolution` SVGA→**HD (3)**, `capture_interval_ms` 1000→**1500**, `cooldown_s` 10→**3**. `capture_count` stays 5, `detect_zoom` stays 0.
- **Deliberately NOT baked as globals:** `detect_zone`, `motion_sensitivity`, `confidence_pct`, `rotation` — per-scene/per-mount, operator-owned.
- **Settings backup/restore:** `GET /api/settings/export` → `birdbox-settings.cfg` (form-urlencoded, byte-identical to the `POST /api/settings` body); Settings-tab **Download/Restore** buttons; restore POSTs the file back, **reusing the existing handler's validation** (no second parser). Durable way to preserve tuning across a wipe.

### 2. SXGA-vs-HD motion investigation + doc — FSD v1.81 (5871d26), no fw change
- User saw degraded motion with an active bird. Diagnosed live: detection wasn't dead — misses were (a) a still/feeding bird absorbed into the rolling background, (b) the 60 s post-boot quarantine. The real degradation was the box being on **SXGA (res=4)**.
- **On-device A/B: HD clearly beats SXGA** — more sensitive AND better-framed. Causes: SXGA's larger frames slow the fixed-period detect loop (misses quick hops), it sits at the exact 1/8 decode ceiling (160×128, zero headroom), and costs ~300 KB more PSRAM (**measured ~547 KB free at SXGA vs ~854 KB at HD** — tied to a spontaneous OOM reboot); plus SXGA's 5:4 FOV is mis-aligned to a horizontal feeder.
- No clean fix on the OV2640 (single stream; res-switch too slow + breaks the ROI box). Documented in **FSD §3.1 + v1.81** and memory ([[motion-detect-res-coupling]]).
- **"Better pics" resolved via JPEG quality, not resolution** — set HD + quality 8 live.

### 3. Resolution selector trimmed to HD + SXGA — 0.62.0, FSD v1.82 (2fcc2fb); Released v0.62.0
- Settings dropdown now offers only **HD (recommended — best motion)** and **SXGA (+vertical detail, weaker motion)**; sub-HD sizes dropped. UXGA stays off the table (no PSRAM for the classifier — verified, `camera.c` comment).
- Help text: HD/SXGA share the same 1280 px width (SXGA only adds vertical rows); HD keeps motion most responsive. Backend unchanged (res clamps 0–4, `res_idx` SVGA fallback); `stLoad` appends a "Current (n)" option for legacy sub-HD values.
- **GitHub Release v0.62.0** created via the repo's CI (`release.yml`): push tag `v*` → CI builds ESP32-S3 with ESP-IDF v6.0.1 → auto-creates "Release v<tag>" with `BirdBox_esp32s3_v<VER>.bin` attached.

### 4. Bug fix — Gallery "Select all" respects the active filter — 0.63.0, FSD v1.83 (dd07e80)
- From `bugreport.txt` bug 1: with a Show filter applied, **Select all** selected *every* image for the day (not the filtered subset) and the count showed the full day, so a follow-up Delete/Recheck could hit hidden images. Cause: `gSelAll()`/counter iterated `gChecks()` (all `.gchk`) ignoring visibility.
- Fix: scope selection to the visible subset — `gSelAll()` toggles only shown tiles (new `gVisChecks()`), and `applyFilter()` **deselects any tile it hides** + refreshes the count. Now **Select all → count == filtered subset**, and Delete/Recheck-selected can only touch visible tiles.
- Verified on the served page (gVisChecks + deselect-on-hide tokens present). Marked `[FIXED in 0.63.0]` in `bugreport.txt` (untracked, not committed).

## Key decisions
- **HD is the one true resolution** (live + compiled default + only-offered choice + documented). Aspect is set-and-forget — ROI-crop makes classification aspect-independent, so we never tune resolution/aspect for the model again.
- **detect_zoom stays a real parameter, default off** — it couples inference preprocessing to *which model is loaded*: off for the whole-frame stock iNat model (now), on for the ROI-trained v0.4 (later). On now would degrade the stock model ([[inat-model-zoom-hurts]]).
- **ROI is logged-not-used:** since 0.60.0 the motion ROI is written to the visit log on every event (banking v0.4 training data), but classification is still whole-frame center-crop until `detect_zoom` flips on with v0.4.
- **UXGA is off the table by design** — two PSRAM walls (motion detect buffer + classifier decode buffer), both verified live. Sacrificing stream smoothness doesn't recover PSRAM. UXGA would only be possible by giving up on-device species ID.
- Deployment-specific settings live in NVS + backup/restore, never hardcoded defaults.
- quality-8 is a live trial, not a committed default — decide after comparing stills.

## Gotchas (still live)
- **Post-boot 60 s quarantine** (`detect_quarantine_s`): right after any reboot/OTA, `n`/`motionTriggers` stay 0 and `quarantineS` counts down — not dead detection.
- **Still bird ≠ motion:** a perched bird triggers on arrival then is absorbed into the rolling background (+ cooldown). Expected.
- **SXGA is the detector's absolute ceiling** — UXGA's 1/8 (200×150) overruns `DETECT_MAX` (160×128) and silently disables motion.
- **Resolution changes need a reboot** (applied at camera_init); quality/rotation/contrast/AE apply live.
- **Whole UI is one inline `<script>`** — one JS syntax error kills all handlers; the C build can't catch it. Verify by grepping the served page (done for every UI change this session).
- **Partial settings POST is safe** — absent fields keep current, zone only applied if 64 chars, strings only if non-empty. So `POST res=3` / `qual=8` don't clobber the user's zone/sens.
- **Build must run from the project root**, not `main/` — running idf.py from `main/` fails ("No project() command") and leaves a stray `main/build`. The build command now pins `Set-Location` to the root.

## What's left to do
- [ ] **User to A/B HD-q8 stills vs SXGA.** If detail is insufficient: Option 2 = keep SXGA but lower `DETECT_PERIOD_MS` (~250→150) to claw back motion (small fw change, testable). Otherwise consider baking quality 8 as the default.
- [ ] **(Optional) Release v0.63.0** — not tagged yet; push tag `v0.63.0` to trigger CI build+release if wanted.
- [ ] **v0.4 ROI-crop retrain** — the main open track. Fork: (a) wait for new ROI-tagged HD captures (accruing since 0.60.0) then export+train cleanly, vs (b) offline ROI re-derivation for the empty-ROI 4-day dataset. Leaning (a).
- [ ] Then: implement `train.py` ROI-crop (replicate `decode_to_input`), retrain **nordic-v0.4**, and **flip `detect_zoom` on in the same step** the v0.4 model deploys.
- [ ] **Data collection is still the real bottleneck** — more diverse Dompap/Lavskrike visits (evening/wide, daytime).
- [ ] Confirm/relabel new gallery visits to seed ROI-tagged training data.
- [ ] Consider restoring `conf` to the user's tuned ~17 if a wipe reset it (user's call).
- [ ] nordic-v0.2 / v0.3 artifacts remain uncommitted interim (not deployable); stock iNat model still runs on the device.
- [ ] `bugreport.txt` is the running bug tracker (untracked) — bug 1 fixed; add new ones there.

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`). OTA: `curl -s -X POST -H "Content-Type: application/octet-stream" --data-binary @build/BirdBox.bin http://192.168.1.111/ota/upload`.
- Build (PowerShell, from repo root): idf_tools.py env-export then `idf.py build` (export.ps1 is broken on this machine — see CLAUDE.md).
- Release: push tag `vX.Y.Z` → `release.yml` CI builds + publishes "Release vX.Y.Z" with the esp32s3 bin.
- Latest commits: `dd07e80` (0.63.0 gallery fix), `2fcc2fb` (0.62.0 res selector), `5871d26` (FSD v1.81 SXGA), `b01f750` (0.61.0 defaults + backup/restore).
