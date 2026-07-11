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

## Full retrain workflow — all steps (FSD §3.2.2)

The complete closed loop, from a bird at the feeder to a smarter model on the
device. Four stages: **A** collect & label on the BirdBox (human, in the web
UI), **B** export the labels to the PC, **C** train, **D** deploy. Stages A–B
run against the device (`192.168.1.111`); C runs on your PC or a Colab GPU (CPU
is fine for a small class set — a few minutes); D uploads back to the device.

`train.py` produces two device-ready files:

- `<OUT_NAME>.tflite` — full-int8, `1x224x224x3`, only the six ops the firmware
  registers, input `zero_point==0` (the firmware feeds `pixel-128` directly —
  `classify.cpp:427`).
- `<OUT_NAME>.txt` — index-aligned labels, `Latin (Common)` per line (localized
  on device by binomial), or a literal `background` guard line.

---

### Stage A — Collect & label on the BirdBox (human classification)

This is the ground truth the whole retrain rests on: **you** decide each
image's species in the web UI. Do it over several days so light, poses, and
individuals vary — variety matters more than raw count.

**A1. Let captures accumulate.** Motion events are saved to the SD card and the
model auto-classifies each one where it can. Lavskrike it *can't* identify (not
in the current model), so those land as "Unidentified" — that's expected, and
exactly why you label them by hand.

**A2. Open the Gallery.** Browse to `http://192.168.1.111`, open the **Gallery**
tab, pick a **day**. Each tile is one capture with the model's guess (if any).

**A3. Know the five states and the Show filter.** Every image is one of:
`0` unclassified · `1` classified (model, unverified) · `2` confirmed species
(human) · `3` no-bird (model) · `4` confirmed no-bird (human). The **Show**
dropdown filters by these: All / Classified / Confirmed / No bird / Confirmed
no-bird / Unclassified / **Near threshold**. Only states **2** and **4** —
things *you* confirmed — become training data.

**A4. Confirm or correct each real bird.** Three actions per tile:
- **✓ Confirm** — the model got it right (e.g. a correctly-named Dompap). One
  click accepts its label → state 2.
- **✎ Relabel** — the model is wrong or blank. Type or pick the species. As of
  firmware 0.48.0 **Lavskrike is a one-click pick** ("Lavskrike (Perisoreus
  infaustus)"); Dompap and the other model species are there too. Picking from
  the list (not free-typing) keeps the binomial attached so every tag exports
  into one stable class folder.
- **🔍 Identify** — re-run the model on a saved photo on demand; it persists the
  decided species as a confirmed label.

**A5. Work the "Near threshold" filter.** This is the rescue queue: real birds
the model ranked #1 but *just under* the confidence threshold, so they were
filed as unclassified. These are the highest-value confirms — especially
female / head-on Dompap (which the model badly under-scores) and anything the
model is unsure of.

**A6. Tag the negatives too.** In the **Unclassified** or **Near threshold**
filter, **double-click a tile to mark it "no bird"** (state 4). These empty /
squirrel / wind frames become the `background` guard class the model needs to
avoid false birds.

**A7. Target volume.** Aim for ~150–300+ confirmed images per species across
different visits/days (FSD §3.2.1). Start with the species at your current
camera position — **Dompap** and **Lavskrike**. Keep going for a few days; a
handful of images per class only proves the plumbing, not accuracy.

### Stage B — Export the labels to the PC

**B1. Pull the confirmed labels** into `dataset/`:

```powershell
powershell -File export-labels.ps1
```

Each confirmed capture lands in `dataset/<class>/` with a `dataset/labels.csv`
manifest. Idempotent — re-run whenever you've labelled more. (For a species the
model can't even rank, `pull-new-captures.ps1` grabs *all* new captures into
`lavskrike/review/` for a by-eye sort instead; see "Two ways data arrives"
above.)

### Stage C — Train the model on the PC

**C1. Check / edit the `CLASSES` table** at the top of `train.py`. Each class
needs its Latin binomial (so `species_i18n.c` localises it on the device) and
one or more image `dirs`, which merge into that class. The defaults are a small
Dompap + Lavskrike proof. Add a `background` class (`dataset/no_bird`, your
Stage A6 negatives) before any real field deployment.

**C2. Create the environment and install deps** (first time only):

```powershell
cd training-data
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

**C3. Train + export:**

```powershell
python train.py
```

Prints the per-class image counts and the train/val split, runs the two
training phases (frozen-backbone head, then a short fine-tune), writes
`nordic-v1.tflite` + `nordic-v1.txt`, then runs the contract check.

**C4. Read the verifier block** at the end. It prints the model's op set, the
input/output dtype + `scale`/`zero_point`, and a final **`RESULT: PASS`** or
**`FAIL`**. Re-run just the checks any time with:

```powershell
python train.py --verify-only nordic-v1.tflite
```

- **PASS** → go to Stage D.
- **FAIL on input `zero_point != 0`** → expected on the first run (int8 PTQ of
  `[-1,1)` data usually yields `zp=-1`). The fix is a ~3-line `classify.cpp`
  change to *read* the input scale/zp (like it already does for the output,
  `classify.cpp:441`). Paste the reported input `scale`/`zero_point` and it can
  be wired in.
- **FAIL on an op outside the six** (e.g. `RESHAPE`, `MEAN`) → either register
  that one builtin in the firmware's resolver or adjust the head; report which
  op it listed.

### Stage D — Deploy to the device

**D1. Upload the model** (only after PASS) — `train.py` prints these two
commands with your names filled in:

```powershell
curl -X POST --data-binary "@nordic-v1.tflite" "http://192.168.1.111/model/upload?name=nordic-v1.tflite"
curl -X POST --data-binary "@nordic-v1.txt"    "http://192.168.1.111/model/upload?name=nordic-v1.txt"
```

**D2. Activate it.** On the device, **Settings → Region / species model** →
select `nordic-v1.tflite`, then reboot. The **Debug** tab's Species-ID card
shows the loaded label count so you can confirm it took. The previous model
stays on the card, so you can switch back if the new one underperforms.

**D3. Close the loop.** Watch the new model on live captures and in the Gallery.
Where it's still wrong, confirm/correct again (back to Stage A) — those new
labels feed the next retrain. Each pass tightens accuracy on your local birds.

> **Reality check for the first run:** with the current tiny/imbalanced set
> (Dompap ~18 vs Lavskrike 126+), this is a *plumbing proof* — does the pipeline
> produce a valid, contract-passing model that loads on the device — not a
> meaningful accuracy result. Keep confirming Dompap to balance it before you
> read anything into the numbers. Scaling to §3.2.1's ~100-150 species is the
> same steps with a longer `CLASSES` table and more data.

## Model versioning & registry

Every build is named `<lineage>-v<MAJOR>.<MINOR>` and ships three files —
`.tflite` (the model), `.txt` (labels), and `.json` (a manifest recording its
identity + provenance so a model on an SD card is never an anonymous blob). The
device selects a model by its `.tflite` filename (Settings → Region / species
model), so the version lives in the name.

**Versions track data provenance, not accuracy** (set `MODEL_VERSION` /
`DATA_PROVENANCE` at the top of `train.py`):

| Version | Meaning |
|---|---|
| **`0.x`** | Pre-baseline experiments — include local BirdBox captures; prove the pipeline. |
| **`1.0`** | First proper baseline trained **purely from external data** (iNaturalist/GBIF stock). The reference — **no device images**. |
| **`1.x`** | `1.0` + *x* rounds of local BirdBox captures mixed in (domain adaptation). |
| **`2.0`** | Breaking change — class set added/removed (label indices shift) or a different architecture/input. |

`train.py` enforces this: a `vN.0` (`MINOR==0`, `N≥1`) refuses to build unless
`DATA_PROVENANCE == "external"`, so a baseline can never be quietly polluted
with local data.

**Registry** — keep this table current as you build models:

| Model | Provenance | Classes | Notes |
|---|---|---|---|
| `inat-v1.0` (stock `inat-birds-v1.tflite`) | external | 965 | Google/Coral iNaturalist-birds, the shipped v1 default (§3.2.1). |
| `nordic-v0.1` | local | Dompap, Lavskrike | First pipeline proof — data-thin, not an accuracy result. |
| `nordic-v1.0` | external | *(planned)* | Pure iNat/GBIF-stock baseline for the Nordic class set. |
