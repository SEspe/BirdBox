# Session Notes — BirdBox (updated 2026-07-16)

Rolling notes for the "BirdBox" work. **This session was entirely off-device**:
evaluating and testing external stock imagery (artsobservasjoner.no via GBIF) as
a retrain data source. **No firmware or FSD change** — firmware stays **0.70.9 /
FSD v2.03**, and the reference unit at **192.168.1.111** still runs the active
model **`nordic-v0.5.tflite`** (unchanged; nothing deployed this session).

One code change landed and was pushed: a `train.py` split improvement
(commit **`7fa9476`**, on `master`, pushed to origin). Everything else from the
experiment is either archived (negative-result write-up) or reverted (the data).

## Headline result — the question we answered

**Does artsobservasjoner/GBIF stock imagery improve species ID on real BirdBox
frames? No.** Controlled A/B, identical held-out birdbox val frames (Bokfink
n=10, Skjære n=9), only the training data varied:

| Model | Bokfink recall | Skjære recall | Overall (n=145) |
|---|---|---|---|
| Baseline (no GBIF) | **90%** (9/10) | 100% (9/9) | **91.0%** |
| + GBIF whole-frame ROI | 50% (5/10) | 100% | 89.7% |
| + GBIF tight bird-box ROI (YOLOv8n) | 70% (7/10) | 89% (8/9) | 88.3% |

Whole-frame hero shots **halved** Bokfink recall (median detected bird = only
**18.5%** of frame → training on ~80% background). Tight-boxing recovered most of
it but **still below baseline**. Conclusion: the barrier is the full **domain gap**
(sensor, IR/lighting, motion blur, feeder background), not just framing — matches
the README's "stock can't close the domain gap" and the earlier iNat "zoom hurts"
finding. **Keep leaning on real BirdBox captures.**

## What we did this session

1. **Evaluated artsobservasjoner.no as an image source.** Don't scrape the ASP.NET
   web UI — the same data (with a Multimedia extension) is mirrored to **GBIF**
   under the "Norwegian Species Observation Service" dataset
   (`datasetKey=b124e1e0-4755-430f-9eab-894f25a9b59c`), queryable via the
   occurrence API. Sanctioned, paged, license-tagged.
2. **Built `training-data/pull-gbif-images.ps1`** — pulls per-species via GBIF
   (`taxonKey` + `mediaType=StillImage` + `country=NO`), license-gates to CC,
   dedups (`gbif-manifest.txt`), and logs per-image attribution
   (`gbif-media.csv`). Pulled **600** images (300 Bokfink `Fringilla coelebs`
   taxonKey 2494422, 300 Skjære `Pica pica` 5229490) into `dataset/<Sci>/gbif/`.
3. **Attribution doc `gbif-ATTRIBUTIONS.md`** — 144 photographers, by
   species/license/contributor. (User confirmed: use all 600, all CC licenses.)
4. **`train.py` split fix (committed `7fa9476`, pushed):** external/un-timestamped
   stock images are now **train-only** (validation measures the birdbox domain,
   not stock), gated by a `BIRDBOX_EXCLUDE_EXTERNAL=1` toggle, with a **per-class
   RNG (`SEED+y`)** so one class's data volume can't shift another's val draw.
   This is what made the A/B airtight. Also gitignored `training-data/*.pt`.
5. **Ran the controlled A/B** (baseline / whole-frame / YOLOv8n tight-box). Boxed
   the 600 hero shots with YOLOv8n (514 detected, 86 no-bird excluded).
6. **Cleaned up (user chose "revert data, keep split fix"):** restored
   `dataset/labels.csv` to pre-GBIF; deleted the throwaway v0.6 experiment models
   (**`nordic-v0.5` remains current**); reset `MODEL_VERSION`→`0.5`/`local`;
   archived the 3 training logs + a `RESULT.md` write-up under
   `training-data/archive-gbif-experiment/`.

## Gotchas & decisions (this session)

- **GBIF, not scraping** — the API is the sanctioned path; images live on
  `artsobservasjoner.no/MediaLibrary/…` but come with per-image license +
  rightsHolder in the Multimedia extension.
- **Per-image licensing** — dataset records are CC-BY 4.0, but each *photo* is
  individually licensed: 465 CC-BY, 18 CC-BY-SA, 117 CC-BY-NC-SA. All fine for a
  private non-commercial model; attribution (the `gbif-media.csv` /
  ATTRIBUTIONS.md) satisfies CC-BY. Filter on the sidecar if this ever goes
  commercial (drop the 117 NC).
- **`train.py` only trains `labels.csv` rows with a valid ROI** (`train.py:193`).
  Images in a class dir do nothing until a labels row + roi exists.
- **PowerShell 5.1 `Invoke-RestMethod` mojibakes å/ø/æ** — GBIF omits
  `charset=utf-8`, so it decodes ISO-8859-1. Fix: fetch raw bytes, decode UTF-8
  ourselves (`Get-GbifJson` in the puller) — else photographer credits are wrong.
- **PowerShell 5.1 `$ErrorActionPreference="Stop"` + native stderr** wraps a TF
  *warning* as a terminating `NativeCommandError` and aborts the script. Use
  `Continue` and gate on `$LASTEXITCODE`; capture logs as UTF-8.
- **NumPy 1.x vs 2.x ABI clash** — installing ultralytics/torch upgraded NumPy to
  2.x, which breaks TF 2.16 (`_ARRAY_API not found`). **Detection needs NumPy 2.x,
  TF training needs `numpy==1.26.4`.** Toggle per step. (Venv currently pinned to
  1.26.4 for TF; re-pin 2.x if running YOLO detection again.)
- **`.gitignore` has no trailing inline comments** — `*.pt   # note` becomes a
  literal pattern and silently doesn't match. Comment on its own line. (Verify
  with `git check-ignore <path>`.)
- **Whole-frame ROI (`0-0-1-1`) is a train/serve skew** for a crop-trained model —
  it was the prime culprit in the whole-frame regression.

## What's left / next steps

- **⚠️ Before the next `train.py` run, set `MODEL_VERSION = "0.6"`** (currently
  `"0.5"`). The committed split change means a rebuild at 0.5 produces a
  *different* model than the committed `nordic-v0.5.tflite` and **silently
  overwrites it** (`OUT_NAME = nordic-v{MODEL_VERSION}`). Bump first.
- **GBIF images retained only as a bootstrap** for a class with *zero* real
  captures (better than nothing to rank an unknown) — not as an accuracy aid
  where real data exists.
- **Untried (low expected value given the result):** down-weight stock to
  ~40/class so it can't swamp; or two-stage pretrain-on-stock / fine-tune-on-
  birdbox. Evidence says stock doesn't help this domain — spend effort on more
  **real** captures instead.
- **Uncommitted working-tree artifacts** (intentionally left untracked; commit if
  wanted): `pull-gbif-images.ps1`, `gbif-ATTRIBUTIONS.md`, `gbif-media.csv`,
  `gbif-manifest.txt`, `archive-gbif-experiment/`. The 600 images and `yolov8n.pt`
  are gitignored. `dataset/` is gitignored as always.

## Device state (unchanged this session)
- **firmware 0.70.9**, active model **`nordic-v0.5.tflite`**, `dzoom:1` (the
  crop-trained model requires detect_zoom ON). Address **192.168.1.111** (confirm
  via `GET /api/status`; CLAUDE.md's `192.168.10.236` is stale).
- Nothing was flashed or deployed this session.
