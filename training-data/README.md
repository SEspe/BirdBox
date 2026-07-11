# Nordic species retrain — local training-data archive

Not part of the firmware (gitignored). Source material for the retrain
scoped in `FSD_BirdBox.md` §3.2.1.

## Two ways data arrives here

1. **Confirmed-label export (`export-labels.ps1`, FSD v1.57)** — the primary,
   automatic path. It pulls `GET /api/labels/confirmed` (every capture you
   verified via the Gallery relabel UI) and drops each image into
   `dataset/<class>/` with a `dataset/labels.csv` manifest. No visual sorting:
   the species was already confirmed by you on the device. Re-run it whenever
   you've relabeled more — it's idempotent (skips files already downloaded).
   This is how the retrain set should grow going forward.

   ```powershell
   powershell -File training-data\export-labels.ps1
   ```

2. **Blind capture pull (`pull-new-captures.ps1`)** — the older, manual path
   for a single species (Lavskrike) before the relabel UI existed: pulls *all*
   new captures into `lavskrike/review/` for a human to sort by eye. Still
   useful for bootstrapping a class the model can't even rank (so it never gets
   a confirmed label to export), but for anything the relabel UI can tag,
   prefer path 1.

## Layout

- `lavskrike/candidate/` — real BirdBox captures visually confirmed as
  Siberian Jay (*Perisoreus infaustus* / Lavskrike), the species that
  prompted this whole effort (see FSD v1.40/v1.41). These are genuine
  domain examples (real motion blur, real edge-clipping, real color
  response) rather than synthetic approximations of them.
- `lavskrike/review/` — newly pulled captures not yet confirmed one way or
  the other; needs a look before moving to `candidate/` or discarding.
- `not-lavskrike/` — pulled captures that turned out to be something else
  (kept in case they're useful negative/other-species examples later, not
  actively curated yet).
- `manifest.txt` — device capture filenames already pulled, so the periodic
  pull doesn't re-download or re-review the same file twice.
- `dataset/` — output of `export-labels.ps1`: one folder per confirmed species
  (`dataset/<Latin_or_common>/*.jpg`) plus `dataset/labels.csv`
  (`file,common,latin,timestamp,relpath`). This is the retrain-ready set; a
  fine-tune script consumes `labels.csv` directly. Regenerated/topped-up by
  re-running the export.

## How it grows

A scheduled routine periodically checks the device (`192.168.1.111`) for
capture files not yet in `manifest.txt`, downloads new ones into
`lavskrike/review/`, and does a first-pass visual sort. Files that are
clearly the same species as the existing `candidate/` set move there
automatically; anything uncertain stays in `review/` for a human look.

This alone won't be enough data for a retrain — variety (different
visits/days/light/individuals) matters more than raw count, and stock
photos from iNaturalist/GBIF (§3.2.1) still supply pose/angle diversity a
single camera position can't. This archive's job is specifically to close
the domain gap (real blur/crop/color) that stock photography can't.

## Training (`train.py`, FSD §3.2.2 Phase B)

Off-device fine-tune that turns the collected/confirmed images into a
device-ready model. Reads the class sources listed in the `CLASSES` table at
the top of `train.py` (the exported `dataset/<class>/` folders plus curated
sets like `lavskrike/candidate/` — several dirs merge into one class), transfer-
learns a MobileNetV2 224 classifier, and exports:

- `<OUT_NAME>.tflite` — full-int8, `1x224x224x3`, only the six ops the firmware
  registers, input `zero_point==0` (the firmware feeds `pixel-128` directly —
  `classify.cpp:427`).
- `<OUT_NAME>.txt` — index-aligned labels, `Latin (Common)` per line (localized
  on device by binomial), or a literal `background` guard line.

```powershell
python -m venv .venv; .\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python train.py                       # trains + exports nordic-v1.{tflite,txt}
python train.py --verify-only nordic-v1.tflite   # re-run the contract checks
```

The last step **verifies** the model against the device contract (op set,
int8 input with `zero_point==0`, shapes) and prints PASS/FAIL — a bad export
is caught here on the PC, not as a silent `AllocateTensors` failure on the
ESP32. On PASS, upload with the two `curl` commands it prints (see
`docs/MODEL.md`), pick the model under Settings → Region / species model, reboot.

Edit `CLASSES` to add species; each needs its Latin binomial (so `species_i18n.c`
localizes it) and one or more image dirs. Add a `background` class (confirmed
no-bird frames) before any real field deployment. Current defaults are a small
Dompap + Lavskrike proof, deliberately tiny to validate the whole loop before
scaling to §3.2.1's ~100-150 species.
