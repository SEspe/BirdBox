# Session Notes — 2026-07-12 ("BirdBox")

A long session spanning firmware features, a full model-retraining investigation, the ROI-crop pipeline groundwork, and FSD documentation. Firmware went **0.56.0 → 0.60.0**; FSD **v1.75 → v1.79** (+ new §3.2.3).

## Firmware shipped (all flashed to the reference unit at 192.168.1.111)

| ver | FSD | what | committed? |
|-----|-----|------|-----------|
| 0.57.0 | v1.76 | Gallery `other` (hard-negative) + `unknown` (excluded) reject states; per-image states 5→7 | ✅ pushed |
| 0.58.0 | v1.77 | Live view: 1s red **border flash** per motion trigger (`/api/motion`) | ✅ pushed |
| 0.59.0 | v1.78 | Live view: highlight the **8×8 grid cell(s)** that triggered (winning cluster); `/api/motion` `"c"` mask | ✅ pushed |
| 0.60.0 | v1.79 | Log motion `roi` **always** (was gated on `detect_zoom`); expose `roi` in `/api/labels/confirmed` | ⚠️ **uncommitted** |

Also: **CLAUDE.md** created (build/flash/verify + gotchas); **session renamed "BirdBox"**; disabled the daily `BirdBox-PullCaptures` scheduled task (legacy puller); parameterized `$Device` in the pull/export scripts (`-Device` > `$env:BIRDBOX_DEVICE` > default .111); **FSD §3.2.3 written** (image→model-input preprocessing contract).

## The model-retraining investigation (the session's core)

1. **Exported** 4 days of confirmed Dompap+Lavskrike → `dataset/` (~1285 imgs, `unknown` excluded).
2. **nordic-v0.2** trained (3-class w/ background reject = no_bird+other). 99% val, PASS — **but failed live**: called a crisp fresh Dompap **Lavskrike 95.7%**.
3. **Root cause 1 — resolution/aspect:** NVS wipe had reset `res` to SVGA (4:3); training data is HD (16:9). Fixed `res`, but…
4. **Root cause 2 (the real one) — train/serve preprocessing SKEW:** `train.py` *stretched* the frame to 224²; device *center-crops* (`classify.cpp` decode_to_input). Model trained on one geometry, served another. Confirmed via `skew.py` (opposite calls under stretch vs crop).
5. **nordic-v0.3** — fixed `train.py`: `center_square()` (matches device) + **visit-grouped leak-free split** + burst de-dup. Honest 98% val. **Still misses** the fresh Dompap (57/43, marginal). De-dup exposed the truth: 603 Dompap imgs = only **~19 visits**. **Verdict: DATA is the wall, not the pipeline.**
6. **Aspect investigation** → concluded **ROI-crop-to-bird** is the real fix for aspect/position/scale invariance (crop a square around the bird, not the frame). Reuses the motion ROI as a free localizer.

## ROI-crop pipeline — groundwork done, retrain BLOCKED, but data clock now started

- **Done:** firmware logs `roi` unconditionally (0.60.0); `/api/labels/confirmed` emits `roi`; `export-labels.ps1` writes a `roi` column; `decode_to_input` crop math + `roi` format captured (`classify.cpp:285-309`; `"x0-y0-x1-y1"` fractional) for replication in `train.py`.
- **⚠️ BLOCKER:** `roi` was only logged when `detect_zoom` was ON — and it's been OFF — so **every historical capture has an empty ROI**. The existing 4-day dataset can't be ROI-cropped. `train.py` ROI-crop and the **nordic-v0.4 retrain are NOT done** (no ROI data yet).
- **✅ LIVE VALIDATION (21:43):** a fresh male Dompap visited at **HD, on the LEFT of frame**. The 0.59.0 grid highlight fired the **correct cells** (left-middle) — localization works. It's the exact hard case (off-center + evening) that center-crop clips and ROI-crop would fix. And being on 0.60.0, **it's the first ROI-tagged HD capture.** Every new visit now accrues ROI training data. (User advised to ✎ confirm it in the gallery → first ROI-tagged training frame + a held-out v0.4 test case.)

## Key decisions

- Reject taxonomy: **a bird can never be a hard negative** — `no_bird`=empty→negative, `other`=cat/sheep→hard negative, `unknown`=unusable bird→excluded.
- v0.3 config: background reject class = no_bird + other merged.
- Motion indicator: keep border flash **and** add per-cell highlight (both).
- ROI logging decoupled from `detect_zoom` (it's metadata, always useful).
- Documented the image→model-input contract in **FSD §3.2.3** (byte-for-byte train==serve rule, center-crop-not-stretch, int8 contract, ROI-crop).

## Gotchas discovered

- **Train/serve preprocessing skew** is the #1 model bug class — cost the entire v0.2 generation. FSD §3.2.3 now guards it.
- **val_acc from a random per-frame split is leakage-inflated** (near-dup burst frames straddle train/val). Use visit-grouped splits.
- **"Motion very hard to trigger":** (a) cluster-size cap (`MAX_CLUSTER_CELLS=20`) rejects *large* motion — a hand at the lens fills too many cells and reads as wind/foliage; small bird-sized motion triggers fine; (b) 60s boot quarantine after every reboot (we rebooted a lot); (c) `sens=94` is already very sensitive, so sensitivity wasn't it.
- **NVS wipe (portal test) reset settings** to compiled defaults — `res`, `conf` (60 vs tuned 17), etc. Re-verify after any wipe.
- Device relocated then returned: reference unit is at **192.168.1.111** again (was temporarily 192.168.10.236). `res` set back to **HD (3)** this session.
- Aspect ratio affects classification because the input is a fixed 224² square; only ROI-crop / letterbox truly remove it. TFLM can't do variable-input (static arena), so the elegant fully-conv fix is off the table on-device.

## What's left to do

- [ ] **Commit + push 0.60.0** (motion.c/classify.cpp/web_server.c roi changes, version.h, FSD v1.79 + §3.2.3, export-labels.ps1 roi column, SESSION_NOTES). *Not yet committed.*
- [ ] **v0.4 data path (FORK):** (a) build **offline ROI re-derivation** for existing frames (motion-diff within bursts, or a small detector) to retrain v0.4 now — won't perfectly match the device ROI; or (b) **wait for new ROI-tagged captures** (now accruing from 0.60.0), then export + train v0.4 cleanly. Leaning (b) now that live visits are logging ROIs.
- [ ] Then: implement `train.py` ROI-crop (replicate `decode_to_input`), retrain **nordic-v0.4**, test with `detect_zoom` on.
- [ ] **Data collection remains the real bottleneck** — more diverse visits (evening/wide Dompap, daytime Lavskrike) regardless of preprocessing.
- [ ] Consider restoring `conf` to the user's tuned ~17 (reset to 60 by the wipe) — user's call.
- [ ] Confirm/relabel the 21:43 Dompap (and further visits) in the gallery to seed ROI-tagged data.
- [ ] nordic-v0.2 / v0.3 artifacts left uncommitted (interim, not deployable). Stock model still running on the device.

## Handy scratchpad scripts (this session)
`infer.py`, `control.py` (harness validation), `contact.py`/`suspects.py` (dataset review), `skew.py` (stretch vs crop), `v3test.py` (v0.2 vs v0.3), `aspect.py` (stretch/crop/letterbox), `livebird.jpg` (the 21:43 HD Dompap).
