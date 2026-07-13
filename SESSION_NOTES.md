# Session Notes — BirdBox (updated 2026-07-13)

Rolling notes for the "BirdBox" work. Firmware is now **0.62.0**, FSD **v1.82**.
All committed and pushed to `origin/master`; flashed to the reference unit at
192.168.1.111.

## Current device state (verified live)
- **version 0.61.0**, clock `ntp`, SD present, free heap ~854 KB.
- **Resolution: HD 1280×720 (res=3)** — set live + locked as compiled default.
- **JPEG quality 8** (`stream_quality=8`, was 12) — trial for crisper stills; live only, NOT baked as default.
- `detect_zoom = 0` (whole-frame center-crop for ID — see below).
- User-owned tuning intact in NVS: `detect_zone` (top 2 rows/sky masked, rightmost col rows 4–6 masked), `motion_sensitivity` (~73), `confidence_pct`.
- OTA preserves NVS, so the live tuning survived the flash.

## What we did this session

### 1. Parameter-defaults cleanup + Settings backup/restore (0.61.0, FSD v1.80) — committed b01f750, pushed, flashed
- **Compiled defaults retuned** so an NVS wipe lands on sane values: `resolution` SVGA→**HD (3)**, `capture_interval_ms` 1000→**1500**, `cooldown_s` 10→**3**. `capture_count` stays 5. `detect_zoom` stays **0**.
- **Deliberately NOT baked as globals:** `detect_zone`, `motion_sensitivity`, `confidence_pct`, `rotation` — per-scene/per-mount, operator-owned. Backup/restore is the durable way to preserve them.
- **Settings backup/restore feature:**
  - `GET /api/settings/export` → downloads `birdbox-settings.cfg` (form-urlencoded blob, byte-identical to the `POST /api/settings` body).
  - Settings-tab **Download settings** / **Restore from file** buttons; restore POSTs the file straight back to `/api/settings`, **reusing that handler's existing validation/clamping** (no second, drift-prone parser).
  - Built clean; UI JS verified balanced (inline-script gotcha).

### 2. SXGA-vs-HD motion investigation + fix (FSD v1.81) — committed 5871d26, pushed
- User reported degraded/absent motion detection with an active bird. Diagnosed live:
  - Detection was **not** dead — events were firing; the "misses" were (a) a still/feeding bird absorbed into the rolling background, and (b) the 60 s post-boot **quarantine** after a reboot. Zone was fine (feeder fully covered).
  - Root cause of the *degradation*: the box was on **SXGA (res=4)**.
- **On-device A/B: HD clearly more sensitive AND better-framed than SXGA.** Mechanisms:
  - *Sensitivity:* SXGA frames are slower to grab+decode → the fixed `DETECT_PERIOD_MS=250` loop samples less often → quick hops missed. SXGA also sits at the *exact* 1/8 decode ceiling (160×128, zero headroom) and costs ~300 KB more PSRAM (**measured ~547 KB free at SXGA vs ~854 KB at HD**) — tied to a spontaneous OOM reboot.
  - *Framing:* SXGA 5:4 is taller/narrower, mis-aligned to a horizontal feeder; HD 16:9 keeps birds in-view.
- **No clean fix on the OV2640** (single stream; per-frame res-switch too slow + its FOV change would break the ROI box). HD locked as default; documented in **FSD §3.1 + changelog v1.81** and in memory.
- **"Better pics" resolved via JPEG quality, not resolution:** set HD + quality 8 live for the user to A/B against SXGA shots. Extra SXGA pixels mostly land on background; ROI-crop maximizes pixels-on-bird from HD anyway.

### 3. Resolution selector trimmed to HD + SXGA (0.62.0, FSD v1.82) — committed 2fcc2fb
- Settings dropdown now offers only **HD (recommended — best motion)** and **SXGA (+vertical detail, weaker motion)**; sub-HD sizes dropped as not useful for bird photos. UXGA stays off the table (no PSRAM for the classifier).
- Help text spells out HD/SXGA share the same 1280px width (SXGA only adds vertical rows) and the motion trade-off. Backend unchanged (res clamps 0–4, res_idx SVGA fallback); `stLoad` appends a "Current (n)" option for legacy sub-HD values.

### 4. OTA + housekeeping
- Rebuilt 0.61.0 clean, OTA-flashed, polled version flip to 0.61.0.
- Fixed FSD version header (was stuck at 1.79 despite the v1.80 entry) → now 1.81.
- Updated memory: [[motion-detect-res-coupling]] gained the "SXGA degrades even when it fits" empirical section.

## Key decisions
- **HD is the one true resolution** now (live + compiled default + documented). Never tune aspect again — ROI-crop makes classification aspect-independent, so resolution is set-and-forget.
- **detect_zoom stays a real parameter, default off.** It couples inference preprocessing to *which model is loaded*: off for the whole-frame stock model (now), on for the ROI-trained v0.4 (later). Flipping it on now would degrade the stock iNat model ([[inat-model-zoom-hurts]]).
- **ROI is logged-not-used:** since 0.60.0 the motion ROI is written to the visit log on every event (banking v0.4 training data), but classification is still whole-frame center-crop until `detect_zoom` goes on with v0.4.
- Deployment-specific settings belong to the operator; backup/restore, not hardcoded defaults, is how they survive a wipe.
- quality-8 is a live trial, not committed as a default — decide after the user compares stills.

## Gotchas (still live)
- **Post-boot 60 s quarantine** (`detect_quarantine_s`): right after any reboot/OTA, `n`/`motionTriggers` stay 0 and `quarantineS` counts down — don't mistake it for dead detection.
- **Still bird ≠ motion:** a perched/feeding bird triggers on arrival, then is absorbed into the rolling background and stops re-firing (plus `cool` cooldown). Expected, not a fault.
- **SXGA is the max the detector can handle at all** — UXGA's 1/8 (200×150) overruns `DETECT_MAX` (160×128) and silently disables motion.
- **Resolution changes need a reboot** (applied at camera_init); quality/rotation/contrast/AE apply live.
- **Whole UI is one inline `<script>`** — a single JS syntax error kills all handlers; the build can't catch it. Verify by bracket/ternary balance + grepping the served page.
- **Partial settings POST is safe:** absent fields keep current values, zone only applied if 64 chars, strings only if non-empty — so `POST res=3` etc. won't clobber the user's zone/sens.

## What's left to do
- [ ] **User to A/B HD-q8 stills vs SXGA.** If detail is insufficient, fall back to Option 2: keep SXGA but lower `DETECT_PERIOD_MS` (~250→150) to claw back motion (small firmware change, testable). Otherwise consider baking quality 8 as the default.
- [ ] **v0.4 ROI-crop retrain** — still the main open track. Fork: (a) wait for new ROI-tagged HD captures (now accruing from 0.60.0+) then export+train cleanly, vs (b) offline ROI re-derivation for the existing empty-ROI 4-day dataset. Leaning (a).
- [ ] Then: implement `train.py` ROI-crop (replicate `decode_to_input`), retrain **nordic-v0.4**, and **flip `detect_zoom` on in the same step** the v0.4 model is deployed.
- [ ] **Data collection is still the real bottleneck** — more diverse Dompap/Lavskrike visits (evening/wide, daytime) regardless of preprocessing.
- [ ] Confirm/relabel new gallery visits to seed ROI-tagged training data.
- [ ] Consider restoring `conf` to the user's tuned ~17 if a wipe reset it (user's call).
- [ ] nordic-v0.2 / v0.3 artifacts remain uncommitted interim (not deployable); stock iNat model still runs on the device.

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`). OTA: `curl -s -X POST -H "Content-Type: application/octet-stream" --data-binary @build/BirdBox.bin http://192.168.1.111/ota/upload`.
- Build (PowerShell): idf_tools.py env-export then `idf.py build` (export.ps1 is broken on this machine — see CLAUDE.md).
- Latest commits: `5871d26` (FSD v1.81 SXGA note), `b01f750` (0.61.0 defaults + backup/restore).
