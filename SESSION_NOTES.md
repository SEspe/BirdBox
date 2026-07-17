# Session Notes — BirdBox (updated 2026-07-17)

Big on-device session. Shipped a **three-tier classification cascade**, trained
and deployed **nordic-v0.6**, added **Nøtteskrike** as a target + training class,
and fixed two bugs. Firmware went **0.70.9 → 0.70.12 / FSD v2.03 → v2.06**. The
reference unit at **192.168.1.111** now runs firmware **0.70.12** with the active
model **`nordic-v0.6.tflite`** (iNat periodic batch **off**, region persists).

(The prior session's GBIF stock-image experiment is archived under
`training-data/archive-gbif-experiment/` and in memory — not repeated here.)

## What shipped (commits on master, all built + deployed + verified)

- `f934198` **0.70.10 / v2.04** — Relabel-picker bugfixes: de-dup Lavskrike
  (now a real nordic class, was emitted twice), and offer the operator's full
  target species list. Added 4 Norwegian names in `species_i18n.c`.
- `9648426` — `train.py` iNat-backbone init option (`BIRDBOX_BACKBONE=inat`) +
  `inat_backbone.py`; bumped MODEL_VERSION → 0.6.
- `beab989` **0.70.11 / v2.05** (Phase A) — cascade reorder: **nordic primary →
  Claude secondary** (only on "Unidentified", constrained to the target
  species); **engine provenance** (`source` visit-log column → `/api/events`
  `src` → gallery `[N]/[C]/[I]` badge); shared `target_species.{h,c}`.
- `2115264` **0.70.12 / v2.06** (Phase B) — **optional periodic iNat batch** via
  runtime model-swap (off by default); **Nøtteskrike** added (target list
  30→31); **region-clobber fix**.
- `1723627` — `train.py`: **Garrulus glandarius (Nøtteskrike) class**;
  MODEL_VERSION → 0.7.

## The three-tier classifier (how an event is labelled now)

1. **nordic-v0.6** (on-device, primary) — ROI-crop, `detect_zoom` ON. Confident
   species / "no bird" kept as-is. `source=nordic`.
2. **Claude** (secondary, when `claude_enabled` + key) — only when nordic returns
   "Unidentified bird"; whole-frame; prompt-constrained to the 31 target species.
   `source=claude`. (Was previously *primary* when on — this session inverted it.)
3. **iNat-965** (optional third tier, **off by default**) — a periodic batch that
   swaps the model, re-scores still-unclassified frames whole-frame masked to the
   31 targets, and swaps back. `source=inat`. Periodic (not live) because only one
   model fits in PSRAM, so a swap costs ~10-13 s SD reload — batching amortizes it.

## Models / retrain

- **nordic-v0.6 (ImageNet backbone, 92.4% val)** trained this session, deployed,
  and promoted over v0.5 (~on par, marginally better). Active on the device.
- **iNat-backbone init A/B = negative result.** Initializing train.py's backbone
  from Google AIY birds_V1 (iNat) weights instead of ImageNet **hurt**: 80.0% /
  79.3% (frozen) vs ImageNet 92.4%, schedule-independent. Tooling
  (`inat_backbone.py`, `.npz`) committed but unused. See memory
  `inat-backbone-init-hurts`.
- **`train.py` is now MODEL_VERSION 0.7 with a 5-species class set** (added
  Garrulus glandarius / Nøtteskrike). Next build = `nordic-v0.7`.
- **Today's confirmed + ROI counts (per species):** Skjære 128, Bokfink 54,
  **Nøtteskrike 32**, Lavskrike 23, Dompap 5 (skipped today), no-bird 37. ROI
  coverage ~99.6% (per-frame ROI logging from 0.70.9 is working). Nøtteskrike at
  32 is thin but a start.

## Gotchas / decisions this session

- **iNat features don't transfer** — backbone init joins GBIF-stock and iNat-zoom
  as documented negatives. Keep ImageNet init + real BirdBox captures.
- **One model in PSRAM** (3 MB arena + ~3.5 MB buffer, ~600-700 KB free heap) →
  the iNat tier must **swap**, hence periodic + opt-in. `model_load_named` /
  `model_unload` reuse the arena (never realloc it); score scratch freed so it
  re-sizes 5↔965 classes. Verified: swap runs, heap stable, restores nordic.
- **Region-clobber bug (fixed):** `h_settings_post` overwrote `g_settings.region`
  even when the field was absent, so any partial settings POST blanked the model
  selection and the device auto-picked the first `.tflite` (`inat-birds-v1`,
  alphabetically) on next boot. Added `has_field()`; empty-but-present (the
  "auto" dropdown option) still honoured. This bit us mid-session (device booted
  into iNat). Always send `region=` on manual settings POSTs.
- **iNat batch on hard frames correctly produces nothing** — its candidates are
  03:xx dark/IR frames nordic also couldn't ID; iNat 30-masked stays below
  threshold. A clear Bokfink scores Fringilla coelebs 14% under iNat, so the path
  is proven; it just declines empty frames.

## Known issues / deferred

- **Visit log is monthly, not per-day (perf).** `visits-YYYY-MM.csv` is read in
  full on every gallery load (~2.2 s SD-bound floor, independent of the day's
  size) and rewritten whole on each relabel (~6.5 s). The fix is **per-day files
  `visits-YYYY-MM-DD.csv`** so no operation touches a month of unrelated rows —
  deferred (documented in FSD v1.98) because it also touches stats / export /
  reset / recheck, **and now the Phase B iNat batch** (which scans the monthly
  file for candidates). Bigger structural change; still open.
- **frameroi sidecar not pruned** — `/log/frameroi-YYYY-MM-DD.csv` (0.70.9) grows
  append-only and isn't cleaned when old captures age out (small, deferred; FSD
  v2.03).
- **iNat batch is coarse** — holds the run mutex for a bounded (25-frame) cycle,
  skips if live events are queued; fine as an opt-in booster, but a busy period
  during a batch delays live ID. Acceptable given it's off by default.

## Device / next steps

- Device: 192.168.1.111, fw **0.70.12**, `nordic-v0.6.tflite`, iNat batch off,
  Claude off (no key). Region persists.
- **Staged, not run:** `train.py` is ready for `nordic-v0.7`; `export-labels.ps1`
  run this session to pull confirmed labels into `dataset/`. Next: `python
  train.py` when ready (iNat-init stays off — ImageNet backbone).
- Not yet updated: README model registry (no v0.7 row until built); the
  `training-data/` model artifacts + logs remain uncommitted (dataset gitignored).
