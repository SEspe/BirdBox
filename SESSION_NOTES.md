# Session Notes — BirdBox (updated 2026-07-16)

Rolling notes for the "BirdBox" work. Firmware is now **0.70.9**, FSD **v2.03**.
Everything committed to `master` (linear history); **0.70.9 is live** on the
reference unit at **192.168.1.111**, flashed via OTA and verified. Nothing this
session is tagged/released yet — **0.69.0 through 0.70.9 are on master only**,
no CI release.

This session pivoted from the previous bug-fix run into the **ROI-crop retrain
loop**: build a Nordic species model that classifies the motion crop (not the
whole frame), plus the gallery/stats plumbing needed to feed it clean training
data. Two model generations shipped to the device this session (**nordic-v0.4
→ nordic-v0.5**), and four firmware releases (0.70.6–0.70.9) support the loop.

## Current device state (verified live)
- **firmware 0.70.9**, clock `ntp`, SD present.
- **Active model: `nordic-v0.5.tflite`** — `clsLabels:5`, `clsRegion:4`,
  **`dzoom:1`** (detect_zoom ON, which the crop-trained model *requires* —
  train and serve must both see the motion box, not the whole frame).
- **`sens` (motion sensitivity) reads 68** — I set it to **50** earlier this
  session and it read back 68 after the v0.5 deploy. **Unresolved anomaly** —
  flag to confirm whether the user moved the slider or something reset it.
- **`conf` (classify no-bird threshold) = 60** — carried over, high; the
  make_decision path calls "background ≥ 60% → no bird", else "Unidentified".
  User's call, not touched.
- **Device address: 192.168.1.111** (confirm via `GET /api/status`; CLAUDE.md's
  `192.168.10.236` is stale — 192.168.1.111 is reality).

## What we did this session (0.70.5 → 0.70.9 + model v0.4 → v0.5)

### Firmware
1. **Log motion ROI on every visit-log row + Maint "Backfill ROI" tool —
   0.70.5, FSD v1.99 (4e2d8a0).** Groundwork so confirmed birds carry a box for
   ROI-crop training; a click-to-place editor backfills boxes on older
   confirmed images that predate ROI logging.
2. **Backfill-ROI editor localized to settings language + Latin — 0.70.6,
   FSD v2.00 (acf4ab7).** The worklist showed the raw `corrected` value (canonical
   English, e.g. "Eurasian Magpie") which *looked* like unconfirmed model output
   but wasn't — `h_roi_todo` only lists human-confirmed rows. Fixed by running the
   name through the same `species_localize(corrected, latin, lang, …)` the
   Gallery/Stats/Live paths use → "Skjære (Pica pica)" under LANG_NO. Server-side
   only.
3. **Gallery visit-label propagation — 0.70.7, FSD v2.01 (bfd52ae).** A motion
   event classifies only its **trigger frame**; the rest of the burst saved
   unclassified, showing as a wall of grey tiles that read as clutter/misses.
   Fix is **display-only** inline JS: walk frames in time order, carry a visit's
   label onto consecutive unclassified frames within **15 s** (`GPGAP`), render a
   dimmed italic "≈ name" badge (`.gprop`), and treat them as their visit's class
   in the Show filters (so "unclassified" no longer lists them). **Never touches
   `st`, the visit log, the export, or Stats counts** — propagation is
   `dataset.prop/psp/pst` only.
4. **Non-destructive statistics reset — 0.70.8, FSD v2.02 (bfd52ae).** "Reset
   statistics" used to **delete every `visits-YYYY-MM.csv`** — the single source
   of truth for *both* Stats *and* every confirmed label + ROI the gallery shows
   and the retrain exports. So a reset silently wiped all training ground truth.
   Fixed with a **reset epoch** instead of a delete: new setting `stats_reset_ts`
   (NVS `s_statrst`); `POST /api/stats/reset` stamps `now` and deletes nothing;
   `stats.c ingest_line` skips rows older than it (ISO timestamps compare
   lexicographically → one `strcmp`). Gallery/export keep reading full history.
   **Guard:** requires an NTP-synced clock (else `now` ≈ 1970 filters nothing) →
   replies `{"ok":false,...}`. Stats tab now shows "N bird visit(s) since <date>".
   `storage_reset_stats()` left defined but unused.
5. **Per-frame ROI logging — 0.70.9, FSD v2.03 (c4b1bba).** Only the winning
   trigger frame recorded an ROI, so confirming a **follow-up** frame (very common
   in multiselect) got a new visit-log row with an **empty** roi → unusable for
   ROI-crop training without hand-backfill. But the boxes already existed
   (`capture_event()` re-runs `detect_once()` before every follow-up so the zoom
   tracks a moving bird) and were being discarded. Fix: `capture_event_frame`
   appends `file,roi` for **every** saved frame (all `capture_count`, not just the
   classified 3) to a per-day sidecar **`/log/frameroi-YYYY-MM-DD.csv`**. On the
   **no-row** relabel case, `storage_relabel`/`_batch` load that sidecar (into a
   PSRAM buffer, taken **before** the write lock → no self-deadlock) and write the
   frame's **own** box into the new row. Graceful for pre-0.70.9 captures (no
   sidecar → empty roi, as before). Sidecar is **not** yet pruned when captures age
   out (small, deferred). **Verified live**: a post-reboot frame relabel recovered
   roi "0.50-0.12-1.00-0.75" matching the bird's position.

### Model (off-device retrain — `training-data/`)
- **train.py reworked to ROI-only, labels.csv-driven (2c21475), then bumped to
  MODEL_VERSION 0.5 (abf6691, this session's last commit).** Crops each image to
  its logged box via `roi_square()` (mirrors the device's `decode_to_input`
  square-expand). Added **Bokfink (Fringilla coelebs)** and **Skjære (Pica pica)**
  classes. **Visit-grouped split** (`VISIT_GAP_S=90`, `MAX_PER_VISIT=10`) keeps a
  whole visit on one side of train/val to stop near-duplicate leakage; prints a
  confusion matrix vs human labels.
- **nordic-v0.4** deployed with dzoom on (Dompap 91%, background 97%; Bokfink /
  Skjære essentially unmeasured — too few).
- **nordic-v0.5** deployed (current). After the user added Bokfink data (53→140
  ROIs): Dompap 91%, Lavskrike 100%, **Bokfink 50%**, Skjære 100%, background 97%,
  **overall 92.4%**. Dompap↔Bokfink confusion is still bidirectional (~4 each way).
  **Root limit: those 140 Bokfink images come from only ~6 distinct visits.**
- Model `.tflite`/`.txt` artifacts stay **gitignored** (reproducible from
  train.py); only the version bump is committed, same as prior models.

## Key decisions
- **Distinct visits >> frames-per-visit for training.** After ROI-cropping,
  positional movement within a visit is normalized away — only pose/angle
  survives, and the individual/background/lighting are held constant. The
  visit-grouped split makes extra frames of a *training-side* visit invisible to
  the val score. So Bokfink improves by capturing **more different landings**, not
  more frames of the same six. (This drove the v0.5 → v0.6 data strategy.)
- **To get all N frames of a visit into training, use "Set species" (batch
  relabel), NOT "Confirm".** Confirm only ratifies frames the *model* classified
  and skips the st0 follow-ups; batch relabel writes the label to every selected
  frame *and* recovers each one's box from the per-frame sidecar. The greyed "≈"
  propagation is **display only** and does not add anything to the dataset.
- **Display-propagation never mutates data** (0.70.7) — real `st`, visit log,
  export, and counts are untouched; propagation is a gallery cosmetic.
- **Stats are a window, not a wipe** (0.70.8) — decouple counters from the
  gallery/training record via a reset epoch; never delete the visit log to
  "reset stats."
- **Load the sidecar before taking the write lock** (0.70.9) — relabel already
  holds the visit-log write mutex, so reading `frameroi-*.csv` inside it would
  self-deadlock.
- **The crop model requires detect_zoom on** — deploying a Nordic model means
  `POST /api/settings` with `region=nordic-vX.tflite&dzoom=1`.

## Gotchas (still live)
- **`POST /api/settings` wipes `region` unless you send it.** The handler sets
  `region` unconditionally (only path-escape guarded), so any partial settings
  POST must include `region=nordic-vX.tflite` (and `dzoom=1`) or it clears the
  model selection. Always send region + dzoom together.
- **Whole UI is one inline `<script>`** — one JS syntax error kills all handlers;
  the C build can't catch it. Grep the served page + bracket/ternary balance pass.
- **The httpd server is single-task** — any handler that blocks (inference, TLS,
  a big SD read/rewrite) freezes the whole UI until it returns. `/api/events`
  (~2.2 s + readdir) is still inline.
- **`python` on this machine hits the Microsoft Store alias** and errors — don't
  pipe device JSON through `python -c`. Use `sed 's/},{/}\n{/g'` + grep, or the
  IDF Python env by full path.
- **Build via the PowerShell tool, not PowerShell-through-Bash** — the Bash tool
  leaks MSYS env vars and `idf.py` rejects with "MSys/Mingw is not supported."
- **OV2640 latches after a soft reboot** — only a hard power cycle clears a
  wedged sensor; an OTA can bring the box up degraded (0.70.2 makes that visible).
- **Cross-signed TLS** (0.70.0) — keep `…CROSS_SIGNED_VERIFY=y`; `certflags 0x0`
  is not proof of a clean cert.
- **sdkconfig.defaults changes need `sdkconfig` deleted** to regenerate.

## What's left to do
- [ ] **Capture more DISTINCT Bokfink visits, then train v0.6.** The single
  biggest model lever — v0.5 Bokfink recall is 50% off ~6 visits. Plan: watch the
  gallery together tomorrow, batch-"Set species" whole Bokfink visits (recovers
  per-frame ROIs automatically now), re-export, retrain.
- [ ] **"1 of 5 / 2 of 5" frame-index indicator in the gallery** — user
  requested; NOT built. N = the frames-per-event `ccnt` setting. Candidate 0.70.10.
- [ ] **Confirm the `sens=68` readback** — I set 50; verify whether the user
  changed it or it reset, then set the intended value.
- [ ] **Batch the bulk Confirm path** — still one `/api/confirm` per image (same
  class as the 0.70.4 relabel fix).
- [ ] **Per-day visit-log files (KNOWN ISSUE, deferred).** Monthly CSV makes both
  gallery load and relabel SD-bound (~2.2 s `/api/events` floor + ~6.5 s rewrite
  per relabel). Split to `visits-YYYY-MM-DD.csv`. Touches write path, `stats.c`,
  `h_labels_confirmed`, reset. (Memory: `birdbox-per-day-log-files`.)
- [ ] **Prune the `frameroi-*.csv` sidecars** when old captures age out (0.70.9
  leaves them; small, deferred).
- [ ] **Release the backlog** — 0.69.0 through 0.70.9 all on master, none tagged.
- [ ] **Add Anthropic API credit, then test cloud ID** (user, deferred).
- [ ] **Consider restoring `conf` 60 → ~17** if the high threshold was a wipe reset.

## Loose ends from this session
- Two test labels I set while verifying per-frame ROI: `19-14-05-033`
  (with ROI, looks like a genuine Dompap — fine) and `19-09-50-692` (no ROI, pre-
  reboot so no sidecar). Minor; left as-is. Revert if they muddy the dataset.
- The 20:47–20:49 evening burst on 2026-07-16 was a Dompap sit-in (no Bokfink).

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`). OTA (firmware):
  `curl -s -X POST -H "Content-Type: application/octet-stream" --data-binary
  @build/BirdBox.bin http://192.168.1.111/ota/upload`. Model files go to `/model`,
  then `POST /api/settings` `region=nordic-vX.tflite&dzoom=1`.
- Build (PowerShell tool, from repo root): idf_tools.py env-export then
  `idf.py build` (export.ps1 broken — see CLAUDE.md). sdkconfig.defaults change →
  delete `sdkconfig`.
- Retrain: `export-labels.ps1` (GET /api/labels/confirmed → `dataset/<Latin>/` +
  labels.csv with roi column; idempotent) → `train.py` → `convert_model_int8.py`.
- This session's commits: `abf6691` (train.py 0.5), `c4b1bba` (0.70.9 per-frame
  ROI), `bfd52ae` (0.70.8 stats decouple + 0.70.7 propagation), `2c21475`
  (train.py ROI-only + Bokfink/Skjære), `acf4ab7` (0.70.6 backfill localize),
  `4e2d8a0` (0.70.5 ROI-on-every-row + backfill tool).
- Prior: `d4a1f6f` (0.70.4 batch relabel + events trim), `be092c5` (0.70.3
  handler offload), `6d4e4fe` (0.70.2 resolution degrade fix).
