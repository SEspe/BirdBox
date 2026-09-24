# Functional Specification Document
## BirdBox — WiFi Nest Box / Feeder Camera with AI Species Identification
**Version:** 3.10
**Author:** SEspe
**Date:** 2026-09-22

This is **the clean, current specification**: what the device is required to do,
stated in the present tense.

It deliberately carries **no history**. The development record — what changed at
each version and why — is in
[`FSD_BirdBox_CHANGELOG.md`](FSD_BirdBox_CHANGELOG.md). Read this file to build
the thing; read that one before reopening a question that was already settled,
because many of those entries record a measurement and a rejected alternative.

---

## 1. System Overview

BirdBox is an open-source, WiFi-connected nest box / bird feeder camera built on an **ESP32-S3 camera module** running **ESP-IDF** (native Espressif SDK — no Arduino framework). It watches a nest box or feeder, detects bird activity, captures photos/short clips, identifies the species with an on-device AI model, and serves everything through its own built-in web UI: live view, capture gallery, and visit statistics.

The device is fully self-contained: no cloud service, no companion app, and no external server is required. Everything — capture storage, species identification, the web dashboard — runs on the device itself, with captures stored on a microSD card. It is designed for public/open-source users: any hobbyist should be able to buy the supported hardware, flash a release binary, join it to their WiFi via the first-boot portal, and mount it on a nest box.

```
 Bird activity ──► Camera + PIR ──► Motion detect ──► Capture (photo/clip) ──► microSD
                                                          │
                                                    Species ID (on-device AI)
                                                          │
 Browser ◄──── Web UI: Live stream | Gallery | Statistics | WiFi | OTA
```

**Development framework:** ESP-IDF v6.x (native Espressif SDK). Project structure, build system (`idf.py`/CMake), WiFi provisioning, web server, OTA and CI conventions follow the proven patterns from the RemoteStart project (`D:\SteinsRootMappe\Claude\RemoteStart`).

---

## 2. Hardware

### 2.1 Target board

| Board | MCU | Camera | PSRAM | Role |
|---|---|---|---|---|
| **Generic "ESP32-S3-CAM"** (N16R8: 16 MB flash, 8 MB octal PSRAM; ESP32-S3-EYE/Freenove-compatible camera pin map) | ESP32-S3 (dual-core LX7, vector instructions) | OV2640 | 8 MB (required) | **Primary target & the project's reference unit** (identified 2026-07-06 by SCCB probe: sensor PID 0x26, camera pin map verified live). Other S3 camera boards (XIAO ESP32S3 Sense, Freenove, S3-EYE) supported via `board_config.h`. |
| AI-Thinker ESP32-CAM (and clones on the same footprint) | ESP32 classic | OV2640 | 4 MB | Secondary/constrained target. The full firmware builds and runs: capture, streaming, gallery, SD and OTA all work, and species ID is not excluded by design — classification is an HTTPS call (§3.2), not an on-device model. What is unproven here is TLS headroom: internal DRAM is smaller than the S3's and the §5 heap guard's floor is correspondingly closer. Camera support is qualified — see the OV5640 note below. |

On the ESP32 classic the camera runs over I2S and cannot DMA into PSRAM, so the CPU copies every frame; that path is specified for the **OV2640**. An OV5640 on this MCU is not supported: it enumerates and streams, but only about one frame in ten assembles into a valid JPEG, and the loss is indifferent to pixel clock and CPU speed. The S3 has no such restriction and drives either sensor.

Exact pin maps are board-specific and defined per-board in a `board_config.h`; the FSD does not fix GPIO numbers. Required peripherals:

| Peripheral | Interface | Purpose |
|---|---|---|
| Camera sensor (OV2640 or better) | DVP via `esp32-camera` component | Photo/clip capture, live stream, motion detection |
| microSD card | SDMMC (1-bit or 4-bit) or SPI, FAT32 | Capture storage, visit log database |
| PIR motion sensor (optional) | GPIO input | Low-power wake / first-stage motion trigger |
| IR LED illuminator (optional) | GPIO output | Inside-nest-box viewing in darkness (requires IR-filter-less sensor variant) |
| Status LED | GPIO output | Boot/WiFi/portal state indication (blink patterns, RemoteStart-style) |

### 2.2 Placement variants

The same firmware supports two mounting scenarios, selected in settings (§5):

- **Nest box mode** — camera inside a closed box, close focus, IR illumination, activity is relatively rare and long-duration (nesting).
- **Feeder mode** — camera pointed at a feeder/perch in daylight, activity is frequent and short (visits of seconds), species variety is high.

The mode primarily tunes motion-detection sensitivity, capture cadence and statistics presentation; the feature set is identical.

---

## 3. Functional Requirements

### 3.1 Motion-triggered capture

- Continuous camera-based motion detection: low-resolution grayscale frame differencing against a rolling background, with configurable sensitivity threshold and a minimum-changed-area filter to reject leaves/light changes.
- **Dominant-cluster trigger with a mount-dependent size cap.** Motion fires on the largest 4-connected cluster of changed grid cells, not the zone-wide total: wind-blown foliage changes many scattered cells a little each, while a bird changes one compact blob a lot. A cluster spanning more than a cap of the 64 cells is discarded outright as wind or foliage rather than merely capped, so a smaller genuine cluster elsewhere in the same frame can still win. How much of the grid a bird covers is a property of the **mount**, not of the bird — at arm's length one fills a third of the frame, at feeder distance a handful of cells — so the cap follows a **camera mount distance** setting (§5): **close 40 cells, medium 28 (default), distant 20**. A cap set for a distant mount silently discards every bird at a close one, and **raising sensitivity makes that worse, not better**: a lower per-cell threshold marks more cells as moved, which grows the cluster toward the cap. When the only cluster in a frame is rejected for size, the device logs it (throttled to once per 5 s), and `GET /api/motion` reports `cap` (the mount's cell cap), `cells` (the last winning cluster's size), `rej` (the largest rejected cluster) and `rejN` (how many frames were rejected this way) — so the discard is visible over the network instead of silent. A climbing `rejN` with a flat `n` is the signature of a cap set too low for the mount.
- **Run HD, not SXGA — detection resolution matters (verified on-device 2026-07-13).** Detection reuses the *same* camera stream as capture (the OV2640 exposes one resolution; there is no separate low-res detect feed), differencing each JPEG decoded at 1/8. **SXGA (1280×1024) degrades detection vs HD (1280×720)** two ways: *sensitivity* — larger frames are slower to grab+decode, so the fixed-period detect loop samples less often and misses quick hops (SXGA also sits at the exact 1/8 decode-buffer ceiling, 160×128, with zero headroom and ~300 KB more PSRAM pressure — measured ~547 KB free vs ~854 KB at HD — which correlated with an OOM reboot); *framing* — SXGA's 5:4 field of view is taller/narrower and mis-aligned to a horizontal feeder, where HD's 16:9 keeps birds in-view. There is no clean fix on this sensor (a dual detect/capture stream isn't available; per-frame res-switching is too slow and its FOV change would break the ROI-crop box). **HD is therefore the locked default (§5, firmware 0.61.0).** SXGA's only advantage — more pixels for the classifier — is dissolved by ROI-crop (§3.2.3), which maximizes pixels-on-bird from HD anyway; recover perceived still sharpness with JPEG quality (lower `stream_quality`) instead. Above SXGA is unusable outright: UXGA's 1/8 (200×150) overruns the detect buffer and silently disables motion.
- Optional PIR pre-trigger: when a PIR sensor is fitted, it acts as the first stage (and, in a future battery variant, a deep-sleep wake source); camera-diff confirms before capture.
- On trigger, the device captures a **visit event**:
  - 1 full-resolution JPEG immediately, plus up to N (default 5, configurable) follow-up frames at a configurable interval while motion persists,
  - optionally a short MJPEG clip (default off — SD write bandwidth permitting).
- Debounce/cool-down (default 10 s, configurable) so one continuous visit produces one event, not dozens.
- Each event is written to microSD as `/captures/YYYY-MM-DD/HHMMSS_<seq>.jpg` plus one row in the visit log (§3.4).
- Retention: configurable cap on SD usage (default 80 %); oldest day-folders are pruned first. Events whose species ID is flagged "favorite" by the user are exempt from pruning.

### 3.2 AI species identification

- Every visit event's best frame is classified on-device by a quantized bird-species model (esp-dl / TFLite-Micro int8, running on the S3's vector unit).
- Model: a compact classifier (MobileNet-class, ~1–4 MB, loaded from SD) trained on garden/feeder species. The model file lives on the SD card so users can swap models/regions without reflashing (Settings → Region, or `POST /model/upload`). **v1 default: Google's iNaturalist-birds model** (MobileNetV2, 965 species worldwide incl. a "background" guard class, Apache-2.0; distributed uint8, converted once to int8 with `tools/convert_model_int8.py` — see `docs/MODEL.md`) — global rather than the originally-planned Northern-Europe-only list, since it's a proven, freely downloadable model that covers the Northern-Europe species; purpose-trained regional models remain the intended upgrade path.
- Output per event: top-3 species with confidence scores; results below a configurable confidence threshold (default 60 %) are labeled **"Unidentified bird"** rather than guessed.
- Species names display in a user-selected language (Settings → Species name language: English or Norwegian; see `species_i18n.c`); the scientific (Latin) binomial is always shown alongside it regardless of language, so identification is never ambiguous.
- A "no bird" class guards against false triggers (squirrels, wind); such events are kept but flagged, and can be auto-pruned.
- The user can correct a wrong label from the gallery; corrections are stored with the event (and can optionally be exported to help improve the shared model — see §9).
- **ESP32-classic fallback:** on non-S3 hardware inference is disabled or limited to bird/no-bird; the UI shows captures as "Unclassified". All other features work identically.
- Classification is asynchronous: capture and storage are never delayed waiting for inference; the label attaches to the event when ready (typically < 5 s later).

#### 3.2.1 Species-model data sources — evaluation, current selection, and regional-retrain path

This section records why the v1 model was chosen and what the realistic upgrade paths are, so the choice is auditable and a future maintainer doesn't re-tread the same search. Researched 2026-07-07.

**The hard constraint** is the device, not the availability of bird data. A drop-in model for BirdBox must be: fully **int8**-quantized (input *and* output), input **1×224×224×3** RGB, built only from the ops the firmware registers (`CONV_2D`, `DEPTHWISE_CONV_2D`, `ADD`, `AVERAGE_POOL_2D`, `FULLY_CONNECTED`, `SOFTMAX`, `PAD` — i.e. MobileNet-family; `PAD` was added in firmware 0.49.0 for Keras MobileNetV2's `ZeroPadding2D`, FSD v1.68), **≤ ~4.5 MB** to fit flash/PSRAM budget with a ~3 MB tensor arena, and shipped with an index-aligned label file. Most public "bird recognition" resources fail one of these before accuracy even enters the picture.

**Sources evaluated and *not* usable as-is (and why):**

| Source | What it is | Why it can't be dropped in |
|---|---|---|
| **BirdNET** (Cornell/TU Chemnitz) | The mature open bird-ID model, ~984 NA+EU species | **Audio**, not vision — needs an I2S mic (out of v1 scope, §11); models are **CC-BY-NC-SA** (non-commercial). Different modality entirely, not a variant of this model. |
| **Merlin Bird ID Photo** (Cornell) | Best-in-class consumer photo ID | **Closed source** — no redistributable model or weights. |
| **NABirds** (Cornell/Visipedia), **CUB-200-2011** (Caltech-UCSD) | Annotated image *datasets* (~400 / 200 species, N. America) | Datasets, not models — research-oriented licensing, no license-clean pre-quantized TFLite export. Would require full retraining, and species coverage is North-American. |
| **OSEA** | Offline tool, claims 10,000+ species, pretrained model | Sized for phone/CLI; adapting to an ESP32's ≤4.5 MB / 3 MB-arena budget is a project, not a swap. |
| **birder** (PyPI) | Apache-2.0 wildlife-CV framework, incl. MobileNet + quantized variants | **PyTorch** — needs a PyTorch→ONNX→TF→TFLite conversion, and *some* bundled weights derive from **CC-BY-NC**. Promising but not turnkey. |
| **Kaggle "450/525 bird species"** trainings (e.g. MobileNetV2, ~96 %) | Community training repos | Training repos over Kaggle datasets, not int8 TFLite; species sets skew to showy/global birds, thin on European garden species; per-uploader licensing is inconsistent. |
| **astrocoding/model-ai-bird-classification-tflite** | A MobileNetV2 **TFLite** bird model — closest in format | Small/undocumented species set, likely float32 (would still need `convert_model_int8.py`), license unspecified. Worth a look only if its label set happens to suit. |

**Current selection (v1 default) — Google/Coral iNaturalist-birds, and why it wins:** it is the only freely available model that already satisfies *every* device constraint at once — MobileNetV2 (ops match), 224×224 RGB, ~3.9 MB after int8 conversion, ships with an index-aligned label file, 965 species covering the Northern-European garden/feeder set plus a `background` guard class, and — decisively — **Apache-2.0**, the only commercially-clean license among the credible candidates. Its one documented weakness is that it's *global*, so it can produce geographically absurd top-1 guesses for anything off-threshold (e.g. "Wild Turkey"/"Greater Roadrunner" surfaced during unrelated testing); the confidence threshold + `background` guard contain this, and it never emits a confident wrong label above threshold (acceptance criterion §12.3). Conversion caveat: the Coral download is uint8 and modern TFLM is int8-only, hence the one-time `tools/convert_model_int8.py` requantization (per-channel symmetric weights, ½-LSB worst case) documented in `docs/MODEL.md` and [[tflm-int8-conversion]].

**Intended upgrade path — a regional retrain, not a different database.** The highest accuracy-per-effort improvement is a **Northern-Europe-subset retrain of this same iNat lineage**, not sourcing a new dataset: same Apache-2.0 provenance, same proven on-device format, but a smaller label set → smaller/faster model and far fewer out-of-region false positives. Sketch of the pipeline (offline, on a PC/GPU — none of this runs on the device):
1. **Species list:** define the target Northern-European garden/feeder/nestbox species (align with the `species_i18n.c` Norwegian table already curated for ~80 species).
2. **Data pull:** fetch labeled images per species from **iNaturalist** (research-grade observations, CC-licensed) and/or **GBIF** occurrence media — filter to the target list and to a sane per-species count.
3. **Transfer-learn:** start from a MobileNetV2 224×224 backbone (ImageNet or the iNat-birds weights), retrain the classifier head (and optionally fine-tune upper blocks) on the regional set, keeping the op set within the seven the firmware registers.
4. **Quantize:** post-training **full-int8** quantization (input+output int8) with a small representative image set — or, if a uint8 export is produced, reuse the existing `tools/convert_model_int8.py`.
5. **Package & install:** emit `<region>.tflite` + index-aligned `<region>.txt`, drop onto SD or `POST /model/upload`, select under **Settings → Region / species model**. No firmware change — the model is data on the SD card by design.

This stays consistent with §9's "shared model improvement" idea (user label-corrections could feed such a retrain) and keeps the device's plug-a-model-on-the-card contract intact.

**Concrete next step — a Nordic/boreal retrain (scoped 2026-07-09).** Prompted by a live miss: the reference unit repeatedly captured a Siberian Jay (*Perisoreus infaustus* — "Lavskrike"), correctly logged every time as "Unidentified bird." Investigation found this isn't a framing/blur/exposure bug (some of the captures were sharp) — the v1 model's 965-species label set simply doesn't contain the species at all (confirmed against the actual Coral label list), and it's also absent from the `species_i18n.c` Northern-European table, so the region filter excludes it too. A GitHub-wide search for a ready-made replacement (any TFLite bird model, any license) turned up nothing better: every alternative found is in the same "~1000 common feeder/garden species" tier, so swapping models wouldn't fix this — coverage of boreal specialists is a gap across the ecosystem, not just this one model. This sharpens step 1-2 of the retrain sketch above into a concrete plan:
1. **Species list:** audit the existing `species_i18n.c` ~80-species table against a broader taxonomy reference for gaps like Lavskrike, and extend it to ~100-150 species covering boreal specialists (Siberian Jay, Siberian Tit, Three-toed Woodpecker, etc.) alongside the existing common feeder/garden set. Google's **SpeciesNet** (`google/cameratrapai`, Apache-2.0, a camera-trap-specific classifier covering 2000+ species incl. Siberian Jay) is a useful cross-check for which species have real camera-trap-viable training signal behind them, but is **not** a usable data or model source directly — it's an EfficientNetV2-M two-model ensemble (object detector + classifier) built for desktop/cloud inference, far outside this device's ≤4.5 MB int8 MobileNet budget, and its 65M-image training corpus isn't published/redistributable (only trained weights are). Checked LILA BC (the camera-trap dataset library behind MegaDetector/SpeciesNet) for a usable Nordic image source directly — found none; its public datasets skew North America/Africa/South America.
2. **Data pull (unchanged in source, sharpened in method):** iNaturalist research-grade observations + GBIF occurrence media remain the realistic image source, filtered to the target species list and a Nordic/Fennoscandian region.
3. **Close the domain gap (new):** every real capture examined today was edge-clipped, motion-blurred, extreme-close-range, or some combination — nothing like the clean centered medium-distance photos iNaturalist/GBIF mostly contain. Training on those as-is risks a model that's accurate in benchmarks but not on this device's actual images. Mitigate by augmenting the pulled images with synthetic motion blur, aggressive edge-clipping crops, and close-range perspective distortion before training, deliberately matching this camera's real capture conditions rather than generic bird photography.
4. **Transfer-learn from the current `inat-birds-v1` checkpoint** (not ImageNet from scratch), retrain the head and optionally fine-tune upper blocks on the expanded regional set — same MobileNetV2 224×224 architecture, same op constraint (seven builtins since 0.49.0).
5. **Quantize/package/install unchanged** (steps 4-5 of the sketch above): `tools/convert_model_int8.py`, `<region>.tflite` + `<region>.txt`, SD card or `/model/upload`, no firmware change. The new model's label set would already be Nordic-scoped, so the region filter becomes a no-op for it rather than needing new logic.
6. **Validate** against already-known-hard real captures (today's Lavskrike and sheep events) before any field redeployment — a concrete regression check rather than only benchmark accuracy.

Not yet scoped: exact per-species image-count targets, training-infra setup (a Colab GPU session is sufficient — training itself is cheap; data collection and the augmentation pipeline are the real effort), and how many of the ~100-150 target species end up obtainable at usable quality from iNaturalist/GBIF — those are only knowable once the data-pull step actually runs.

**Shipped as a lighter-weight alternative to a retrain — the region *filter*.** Rather than a new model, **Settings → Species set → Northern Europe only** restricts the *existing* global model's output to the ~80-species Northern-European set (the same `species_i18n.c` list, matched by Latin binomial): at inference the ranking skips any class whose binomial isn't in the set (the `background` guard and any no-binomial label are always kept), so the model physically can't return an out-of-region species. Confidence is deliberately **not** renormalized — an out-of-region animal simply loses its winning class and the best in-region score falls below the threshold, landing as "Unidentified bird" instead of a confident wrong guess. It's a decision-layer restriction, so inference speed and in-region accuracy are unchanged; it only removes out-of-region false positives (the "Wild Turkey at a Norwegian feeder" failure mode). Robustness: the filter auto-disables for a model whose labels don't match the set (0 in-region labels → no filtering), so pairing it with an unrelated model can't blank every result; the Debug tab reports the in-region label count. Default off (global), so the device ships region-neutral. This is the pragmatic first rung of the retrain ladder above — most of the false-positive benefit, none of the training infrastructure.

#### 3.2.2 Closed-loop retraining pipeline (human-in-the-loop) — end-to-end feature and build plan

Scoped 2026-07-11. §3.2.1 records *which* model and *why*, and sketches the offline retrain in the abstract. This section formalizes the **operational loop** that turns everyday use into model improvement: a human confirms species on the device → those confirmations become a labeled dataset on a PC → a fine-tune produces an improved model → it is quantized and pushed back to the SD card. Each pass tightens accuracy on the local species set under this device's real capture conditions — the domain gap stock photography can't close (§3.2.1 step 3). It is the concrete realization of §9's "user label-corrections feed the shared model," kept fully local (no telemetry; nothing leaves the device automatically).

**Four stages, and what already exists.** Three of the four are built; the fine-tune in the middle is the gap.

| Stage | Mechanism | Status |
|---|---|---|
| 1. Human label collection on the device | Gallery ✎relabel / ✓confirm / 🔍identify write the `corrected` column; 5 per-image states; **Near threshold** rescue filter (§3.4/v1.65); double-click = no-bird. Images already on SD as captures. | **Built** (fw 0.47.1) |
| 2. PC-side retrieval | `GET /api/labels/confirmed` → `training-data/export-labels.ps1` → `dataset/<class>/*.jpg` + `labels.csv`; idempotent. `pull-new-captures.ps1` for blind pulls. | **Built** |
| 3. Off-device fine-tune | — | **Not built** (core new work) |
| 4. Quantize + install + close loop | `tools/convert_model_int8.py` (uint8→int8); `POST /model/upload` accepts `<region>.tflite` + index-aligned `.txt` into `/sd/model`; no firmware change. | **Partial** — install path exists; validation, versioning, and label-map regen do not |

**The new work (gaps):**
1. **Training script (stage 3, the core).** Transfer-learn from the `inat-birds-v1` MobileNetV2 224×224 checkpoint (not ImageNet from scratch), retrain the head and optionally upper blocks on the merged set (device `dataset/` + iNat/GBIF stock for pose diversity, §3.2.1 steps 2-3), with the capture-condition augmentation of §3.2.1 step 3 and class-weighting for the inevitable per-species imbalance. Must stay within the seven registered ops (§3.2.1) and the ≤4.5 MB / ~3 MB-arena budget. Runs offline on a Colab GPU.
2. **Label-map / model contract (stage 4).** The device maps output index → name from the index-aligned `.txt` sidecar, and `species_i18n.c` maps those to Norwegian. A retrain — especially a reduced Nordic class set — reorders classes, so the `.txt` **and** the `species_i18n` entries must be regenerated from the *same* class list the training used. This coupling is the sharpest correctness risk: a mismatched label file mislabels every inference silently.
3. **On-device validation harness (stage 4).** Before trusting a candidate, run it over a held-out set of already-labeled real captures via `POST /api/classify` and compare top-1 accuracy against the current model (a concrete regression check per §3.2.1 step 6, not just benchmark accuracy). Gate the install on it.
4. **Model versioning / rollback (stage 4).** Keep the prior `.tflite` so a regressive retrain reverts — mirrors the OTA rollback posture (§8).
5. **Stage 1/2 polish (optional).** Guard human-labeled captures against retention pruning (§7) so training candidates aren't deleted before export; a scheduled `export-labels` refresh instead of manual runs.

**Build plan — prove the loop small first.** The recommended sequence front-loads the end-to-end proof over species breadth:
- **Phase A — data + decision.** Choose the class set; start deliberately tiny (e.g. Dompap ♂/♀, Nøtteskrike, Lavskrike, `no_bird`). Build the stock-image fetcher and the dataset assembler (merge device+stock, train/val split).
- **Phase B — training.** Get a float model that beats stock on the held-out val set.
- **Phase C — quantize + contract.** Int8 convert; regenerate `.txt` + `species_i18n` from the class list; op/size check.
- **Phase D — validate + install.** Run the on-device harness; if better, `/model/upload`; retain the old model.
- **Phase E — ergonomics (optional).** Scheduled pull; pruning protection.

A throwaway ~3-class proof through Phases A-D validates the whole pipeline (and de-risks the label-map coupling and the int8 accuracy drop) before scaling to the ~100-150 species of §3.2.1.

**Effort estimate.** Engineering is small; **data volume is the long pole.**

| Item | Focused-work effort |
|---|---|
| Class-set decision + label-contract design | 0.5 d |
| Stock-image fetcher (iNat/GBIF, dedup, baseline-JPEG) | 1 d |
| Dataset assembler (merge, split, class weights) | 1 d |
| Training script (transfer-learn, augmentation, op-constraint) | 1.5-2 d |
| Quantize + representative-set wiring (converter exists) | 0.5-1 d |
| Label `.txt` + `species_i18n` regeneration from class list | 0.5 d |
| On-device validation harness (`/api/classify` over held-out set) | 1 d |
| Model versioning / rollback + docs | 0.5 d |
| Automation + pruning-protection (optional) | 0.5-1 d |
| **Engineering total** | **≈ 6.5-8.5 days** |

The gating cost is **not** engineering: a usable retrain needs on the order of **150-300+ confirmed images per priority species across varied visits, light, and individuals** (variety over raw count). At the reference unit's current capture/label rate that is **weeks of passive collection plus human labeling time** — realistically 3-8 weeks to a first usable Nordic set, though the 3-class proof can start with far fewer. This matches §3.2.1's "data collection and the augmentation pipeline are the real effort."

**Risks:** (1) label-map/model coupling — regenerate `.txt` + `species_i18n` together or every label is silently wrong; (2) int8 quantization can erode the fine-tune's gain — the validation harness is the guard; (3) label noise (e.g. the fat-fingered `"dom"` typo-class, v1.51-53) needs a cleaning pass in the assembler; (4) a fine-tuned graph must not introduce an op outside the registered seven (`train.py`'s verify step checks this — it's how `PAD` was caught).

#### 3.2.3 Image → model-input contract (how a picture becomes classifier input)

**This is the single most skew-prone part of the pipeline. The rule is absolute: the preprocessing that turns a JPEG into the model's input tensor must be byte-for-byte the *same operation* during training (`train.py`) and on-device inference (`classify.cpp` `decode_to_input`). Any divergence silently mistrains/misclassifies — it cost a whole model generation (v0.2), see below.**

The model input is a **fixed `1×224×224×3` int8 tensor**. A camera frame of any resolution/aspect must be reduced to exactly that. The contract has three stages, identical on both sides:

1. **Decode to RGB.** Device: `esp_jpeg` decodes the JPEG at a power-of-two downscale (1/2…1/8) chosen so the crop region still exceeds 224 px (baseline JPEG only). Training: `tf.io.decode_image(..., channels=3)`. Both yield RGB.
2. **Geometry — crop a square, never stretch.** Take a **square region** of the frame and resize it to 224×224 with **nearest-neighbour** sampling (`sx = x*side/224`). The square is:
   - **Whole-frame mode (`detect_zoom` off, the default):** the largest **centered** square, `side = min(w,h)` — the classic center-crop. This is **aspect/resolution independent** (a centered square is the same shape whether the source is 16:9, 4:3, or 5:4). ***Never* squash the full rectangle into the square** — squashing distorts the bird's proportions differently per source aspect and is the v0.2 skew (see below).
   - **ROI mode (`detect_zoom` on):** a square centered on the **motion ROI** (`motion.c`'s winning cluster, logged in the visit-log `roi` column as `"x0-y0-x1-y1"` fractional), `side = max(roi_w, roi_h)` clamped to the frame, so the bird fills the input regardless of where/how big it was in frame. Exact math: `classify.cpp` `decode_to_input` (`rx0 = roi.x0*w+0.5`, square-expand on ROI center, clamp to frame). Training must crop each image to its logged ROI with the same math.
3. **Quantize to the int8 contract.** Input tensor is **int8, zero_point 0, scale 1/128**. The firmware feeds `pixel ^ 0x80` (== `pixel − 128`) straight in and does **not** apply the tensor's scale/zero-point (`classify.cpp:420-427`). So the model must be trained on `x = (pixel − 128) / 128 ∈ [−1, 1)` and quantized so the input tensor has `zero_point == 0`. Output is int8; the head stays within the **seven registered builtins** (`CONV_2D, DEPTHWISE_CONV_2D, ADD, AVERAGE_POOL_2D, FULLY_CONNECTED, SOFTMAX, PAD`) and the ~4.5 MB / ~3 MB-arena budget. `train.py`'s verify step asserts input dtype/shape/zp, the op set, and output dtype before an artifact is allowed to ship.

**Why this section exists (the v0.2 lesson).** `train.py` originally *stretched* the whole frame to 224² (`tf.image.resize`) while the device *center-cropped* it. The model learned stretched geometry, was served center-cropped geometry, and confidently misclassified a clear off-aspect Dompap as Lavskrike — while validating at a fake 99% because the leaky per-frame split hid it. Fixed in the v0.3 `train.py` by replacing stretch with the device-matching `center_square()`. Corollary rules:
- **Rotation** (§5): 0/180° are baked into the saved JPEG; 90/270° are applied as an index permute in `decode_to_input`. Training crops in raw-frame axes, so a 90/270° *display* rotation would reintroduce a train/serve mismatch — retrain at the deployed rotation, or in raw-frame space.
- **Resolution is a training-data contract, not a quality knob.** Extra pixels never reach the model (everything ends at 224²); what matters is that capture aspect matches what the model was trained on. Center-crop makes the model aspect-independent; ROI-crop additionally makes it position/scale independent. After any NVS wipe, re-verify `res` (and `sens`/`zone`/`conf`) — a reset to a different resolution/aspect silently degrades classification.
- **ROI availability:** the ROI is only useful for training if it was logged — logging was decoupled from `detect_zoom` in fw 0.60.0, so only captures from 0.60.0 onward carry a `roi`; earlier frames are whole-frame-only.

### 3.3 Live streaming / remote viewing

- MJPEG live stream over HTTP (`/stream`), viewable in any browser on the LAN — embedded in the web UI's Live tab and usable as a direct URL (e.g. as a camera source in Home Assistant/VLC).
- Configurable stream resolution/quality, independent of capture resolution; default 800×600 to keep capture quality unaffected.
- At most 2 concurrent stream clients (memory bound); further clients get HTTP 503 with a clear message.
- Streaming and motion detection coexist: while a client streams, motion detection keeps running (detection uses its own low-res frames).
- Remote (outside-LAN) viewing is explicitly **out of scope for the device itself** — no cloud relay, no port-forwarding automation. The documented path is the user's own VPN (WireGuard/Tailscale) to their LAN. This is a deliberate security decision for an open-source device.

### 3.4 History & statistics

- Every visit event is appended to a visit log on SD (`/log/visits.csv`, append-only, one file per month): timestamp, species, confidence, frame count, file paths, user correction.
- **Gallery tab:** browse captures by day; each event's first frame is badged with its species + confidence (joined from the visit log by frame path, localized to the display language); view full-size frames, delete a single capture, multi-select + delete, delete a whole day (photos only), or **wipe a day** (photos + that day's statistics in one action). Favorites (pruning-exempt) and in-gallery label correction remain deferred.
- **Statistics tab:**
  - visits per day/week/month (bar chart),
  - species leaderboard (distinct species, visit counts, first/last seen),
  - activity-by-hour-of-day profile,
  - "new species" flag when a species appears for the first time.
- Charts are rendered client-side in the web UI from JSON APIs (§6); the device only serves data, keeping firmware HTML small (RemoteStart pattern: single-file embedded UI, no external CDN dependencies — the UI must work with no internet access).
- Time base: SNTP, server configurable in Settings (default `pool.ntp.org`), timezone configurable (default `Europe/Oslo`, auto-DST). Capture files are date-time named (`YYYY-MM-DD_HH-MM-SS.jpg`) under a `YYYY-MM-DD/` day-folder.
- **Offline / no-NTP fallback (§3.4.1):** on a network without a reachable NTP server (no internet uplink, or UDP-123 blocked), SNTP never syncs and the clock stays at ~1970 — so captures fall back to `/captures/no-date/`. To avoid this, the web UI posts the **browser's clock** to `POST /api/time` on page load and before every snapshot; if (and only if) SNTP hasn't synced, the device sets its clock from it (`settimeofday`, sanity-checked > 2023) and re-applies the timezone. SNTP remains authoritative — a browser clock never overrides an already-synced time. Any captures already stranded in `no-date/` from before the clock was set stay there (harmless); new captures are correctly dated. The current device time and its source (`ntp`/`manual`/`none`) show in the live-view status line and the Debug tab (`time`/`clockSrc` in `/api/status` + `/api/sysinfo`).
- The timezone (`g_settings.timezone`, POSIX form) is applied at boot right after settings load — before any capture path uses `localtime` — not only on WiFi connect, so date folders/filenames and the displayed time are in local time from the first frame.

### 3.5 Camera watchdog & recovery

**Problem (observed in the field, firmware ≤0.18.0):** after several hours of running, the OV2640 can stop delivering frames — `esp_camera_fb_get()` returns `NULL` on every call while SCCB still reads the sensor PID, so `camPresent`/`camPid` look healthy but the live stream and captures are dead. The stall survives a soft `/api/reboot` (the ESP32 restarts but the sensor stays powered and latched); only removing the sensor's VDD clears it. Confirmed live: a battery disconnect restored video where a reboot did not.

**A worse failure mode, and the design lesson (learned in live test of the first cut).** As the reference unit's camera degraded, a second mode appeared: instead of returning `NULL`, `esp_camera_fb_get()` **spins CPU-bound** on corrupt frames (`cam_hal: NO-SOI` in the log). The first watchdog cut (a) held a camera mutex *across* `fb_get()` and (b) ran at low priority, started *after* `motion_start()`. Both were fatal: the spinning grab (in the prio-4 motion task) starved the prio-1 `app_main` before it ever reached `camera_watchdog_start()`, so the watchdog never launched; and even if it had, the mutex held by the spinning grab would have blocked recovery forever. So the broken camera prevented its own recovery. The redesign below fixes both.

**Detection.** All frame grabs go through `camera_grab()`/`camera_return()` (never `esp_camera_fb_get()` directly). These **never hold a lock across `fb_get()`** — a tiny critical section only guards a `recovering` flag (new grabs bail so none enter `fb_get()` during a deinit), an `inflight` count (grabs inside `fb_get()` or holding an un-returned frame), an `attempts` counter, and the last-good-frame timestamp. The `cam_wd` task checks every 2 s and declares a stall when no good frame has arrived for >5 s **while consumers are actively grabbing** (attempts advancing or a grab in flight). If nobody is grabbing, the heartbeat is stale but the camera is merely *idle* — left alone. Crucially the watchdog does **no probe grab of its own**, so a spinning camera can't wedge the watchdog too.

**Startup ordering & priority (the fix for "watchdog never started").** `camera_watchdog_start()` runs right after `camera_init()` and **before** `wifi`/`web`/`motion` in `main.c`; rollback-validate and "boot complete" also run before `motion_start()`, which goes **last**. So everything critical is up before the one task whose grab loop can spin on a broken camera. The `cam_wd` task runs at **priority 6 — above the motion task (4)** — so it stays schedulable to detect and recover even while a grab spins CPU-bound.

**Recovery.**
- **Soft re-init (implemented).** Set `recovering` (new grabs bail), drain `inflight` (bounded 2 s), `esp_camera_deinit()`, hold **XCLK low ~150 ms** to let the sensor PLL settle, re-init. Clears ESP32-side LCD_CAM/DMA latches, XCLK glitches, and the `fb_get()`-returns-`NULL` latch. Counted + timestamped.
- **Undrainable wedge → fault (implemented).** If `inflight` won't drain (a grab is stuck spinning inside the driver), `deinit()` would crash that task, so recovery **cannot proceed safely** — it returns `ESP_ERR_INVALID_STATE` and the watchdog raises `camFault` and backs off. Auto-reboot is *deliberately not* used: field evidence shows a full reboot doesn't clear the sensor-core latch, so it would only drop recordings for nothing.
- **Repeated failure → fault (implemented).** If three soft re-inits in a row don't bring frames back, set `camFault` and back off. Either way the fault is surfaced loudly (Debug tab + `/api/sysinfo`) so the operator knows a manual power cycle is required.

**Telemetry.** `/api/sysinfo` exposes `camRecoveries` (successful re-inits since boot), `camRecoveryAgo` (seconds since the last, −1 if none), `camFault` (bool), and `socTempC` (ESP32-S3 on-die temperature — a proxy for whether the box is running hot, for the overheat-vs-connection diagnosis; the sensor is the SoC die, not the camera). The Debug tab shows "Watchdog recoveries", a color-coded "SoC temperature", and a red "Camera fault" row when set.

**Tier 2 — hardware power-cycle (NOT implemented; requires a HW mod).** The guaranteed fix for the analog-core latch is to cut and restore the sensor's power, reproducing the battery pull in firmware. The reference board (`BOARD_ESP32S3_CAM_GENERIC`) has **`CAM_PIN_PWDN = -1` and `CAM_PIN_RESET = -1`** — neither line is wired to a GPIO, so firmware has no power lever today. Two possible mods, in increasing reliability:
  1. **Wire PWDN** to a spare GPIO and set `CAM_PIN_PWDN`. On many OV2640 modules PWDN gates the internal regulators; toggling it *may* be enough. Cheapest (one wire), not guaranteed.
  2. **MOSFET / load-switch on the camera VDD rail** driven by a spare GPIO (P-FET high-side switch or a load-switch IC). Literally power-cycles the module — guaranteed to clear the latch.

  Once either lever exists, tier 2 slots into `camera_recover()` between tiers 1 and 3: assert power-off → wait ~300 ms → assert power-on → re-init.

---

## 4. WiFi Configuration (First Startup)

Identical in design to the RemoteStart project's provisioning (its FSD §4 / `main.c` WiFi section), reimplemented in this codebase:

1. On first power-up — or any boot where NVS holds no WiFi credentials — the unit opens a SoftAP config portal:

   | AP Name | Password |
   |---|---|
   | `BirdBox-Config` | `birdbox1234` |

2. The portal page (served at the AP's root, `192.168.4.1`) offers a **WiFi network scan** — the user picks their SSID from the scanned list (or types a hidden one) and enters the password.
3. Credentials are URL-decoded and saved to NVS (`ssid`/`pass` in the `birdbox` namespace); the unit reboots and connects as a STA.
4. Boot-time connect: 15 s timeout / 5 retries. On failure **with stored credentials**, the unit reopens the portal *in APSTA mode* while continuing to retry in the background (a nest box may be at the edge of range; the portal must not permanently strand a temporarily-offline unit). Once connected the first time, in-service disconnects retry indefinitely and never reopen the portal (RemoteStart v1.25 lesson).
5. A **WiFi tab** in the normal web UI (always available, not just first boot) allows changing SSID/password and choosing **DHCP or static IP** (IP/mask/gateway/DNS, server-side IPv4 validation, invalid saved config falls back to DHCP) — the RemoteStart v1.37/v1.38 design.
6. Credentials reset: hold the boot button ≥ 5 s at power-up → NVS WiFi namespace erased → portal reopens.
7. `WIFI_PS_NONE` and max TX power from day one (RemoteStart v1.32 lesson — modem-sleep latency ruins HTTP streaming), and `httpd` with `lru_purge_enable = true` and handler-count headroom (v1.35/v1.27 lessons).

### 4.7 Alternative network (alt1) & failover

The device stores **two** networks: a **primary** (`ssid`/`pass`) and an optional **alternative "alt1"** (`ssid2`/`pass2`), both configured from the WiFi tab (each with its own scan-and-pick). This covers a box that can see two APs (e.g. a house AP and a garden repeater on a different SSID), so it isn't stranded when one is down or out of reach.

- **Boot:** try the primary first (15 s / 5 retries); if unreachable, try alt1 (same budget); if both fail, open the portal — which then keeps retrying **both** networks, alternating each 60 s cycle.
- **In service:** after 5 consecutive reconnect failures on the current AP, fail over to the other stored network, and keep alternating — so the box latches onto whichever AP is actually present as it roams or as APs come and go. Never reopens the portal once ever-connected.
- **Shared IP setting:** the DHCP-or-static IP configuration (§4.5) applies to **whichever** network connects. A single static address is only valid if the primary and alt1 are on the **same subnet**; for APs on different subnets, use DHCP. The WiFi tab states this.
- **Visibility:** the WiFi tab shows the currently-connected AP plus both configured SSIDs (`GET /api/wificfg`, passwords never exposed); the Debug tab's WiFi Link section shows the connected network name (`apSsid` in `/api/sysinfo`).
- **Editing:** saving with `slot=1` and a blank SSID removes alt1. The boot-button reset (§4.6) erases both networks. Passwords are write-only — the tab shows SSIDs but never pre-fills passwords.

### 4.8 mDNS — `http://birdbox.local/`

The unit runs an mDNS responder (`espressif/mdns`) once STA-connected, so the UI and OTA are reachable by name instead of by whatever address DHCP handed out (the reference unit has changed subnets before):

- **Hostname:** `birdbox` (compile-time `MDNS_HOSTNAME` in `version.h`) → `http://birdbox.local/`. The same name is also set as the **DHCP hostname**, so the box appears as "birdbox" in router client lists (and routers that publish local DNS names resolve it LAN-wide).
- **Service advertisement:** `_http._tcp` port 80, instance "BirdBox camera" — network/Bonjour browsers list the device.
- **Best-effort:** mDNS start failure logs a warning and never blocks bringup; the IP path is unaffected. Not started in portal mode (the portal has its own fixed `192.168.4.1`).
- **Scope caveat (by design of mDNS):** name resolution is same-subnet multicast; a client on a different subnet needs the router to reflect mDNS, or uses the IP / router DNS name. Client support: Windows 10+, macOS, iOS, Android 12+ resolve `.local`; older Android may not.

---

## 5. Web UI

Single-page UI embedded in firmware (no filesystem-served assets, no CDN), tab bar following the RemoteStart standard:

**Live | Gallery | Stats | Settings | Debug | WiFi | OTA Update**

- **Live** — MJPEG stream, snapshot button, current motion-detection state indicator, quick rotation toggle (mirrors the Settings tab's rotation field).
- **Gallery** — §3.4 browsing/labeling.
- **Stats** — §3.4 charts, plus a confirm-gated Reset Statistics button that clears the visit-log history (saved photos are unaffected).
- **Settings** — placement mode (nest box/feeder), motion sensitivity, camera mount distance (close/medium/distant, §3.1), capture count/interval, cool-down, confidence threshold, species set (global / Northern-Europe filter, §3.2.1), retention cap, stream quality, camera resolution (HD or SXGA, reboot to apply), contrast, image rotation (0/90/180/270, mount-correction), species-model region (§3.2), timezone, NTP server, IR LED mode (off/auto), and a System Monitoring section carrying the Home Assistant / MQTT settings (§13), and a Night Sleep section (§14).
- **Debug** — System card (free heap + low-water mark with age, uptime, WiFi reconnect count + last-reconnect age), WiFi Link card (RSSI/channel/own MAC), SD card status (size/free/health), camera sensor status, last-inference timing.
- **WiFi** — §4 step 5, plus a Reboot Now button.
- **OTA Update** — §8.

All settings persist in NVS and apply without reflashing; settings that require a restart (camera resolution) say so and offer the reboot.

---

## 6. REST API

All UI data flows through JSON endpoints, so the device is scriptable/integrable (Home Assistant etc.) without scraping:

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status` | GET | Current state: motion, last event, species, SD/heap/WiFi summary |
| `/api/sysinfo` | GET | Debug-card data (heap, heapMin+age, uptime, reconnects) |
| `/api/events?date=&species=&page=` | GET | Paged visit log |
| `/api/events/<id>` | PATCH / DELETE | Correct species label, favorite, delete |
| `/api/stats/daily`, `/api/stats/species`, `/api/stats/hourly` | GET | Chart data |
| `/api/stats/reset` | POST | Delete all visit-log CSVs (clears stats/history; photos untouched) |
| `/api/captures/delete` | POST | Bulk-delete photos: `date=` + `files=a.jpg,b.jpg` (multi-select) or `all=1` (whole day); add `stats=1` to also wipe that day's visit-log rows |
| `/api/capture` | POST | Manual snapshot now |
| `/api/settings` | GET / POST | Read/write settings |
| `/api/ha-status` | GET | Live state of the Home Assistant MQTT client: enabled, connected, updates sent, last error (§13) |
| `/api/night` | GET / POST | Night-sleep state (mode, asleep, seconds asleep, last luma); POST wakes now and holds off re-sleeping (§14) |
| `/api/ipconfig`, `/api/ipconfig/save` | GET / POST | DHCP/static IP (RemoteStart design) |
| `/api/reboot` | POST | Reboot |
| `/stream` | GET | MJPEG live stream |
| `/captures/...` | GET | Static serving of stored JPEGs from SD |
| `/ota/upload` | POST | Raw `.bin` firmware upload (§8) |
| `/api/classify` | POST | Classify a posted JPEG now — top-3 + decision (§3.2) |
| `/api/classify-file?date=&f=` | GET | Re-run species ID on a saved Gallery photo, read from SD (§3.2) |
| `/model/upload?name=` | POST | Install a model/labels file into `/sd/model` (§3.2) |

No authentication in v1 (LAN-only device, same posture as RemoteStart); an optional basic-auth password is a v2 candidate and noted in §11.

---

## 7. Storage

- **microSD (FAT32)**: captures (`/captures/YYYY-MM-DD/`), visit logs (`/log/visits-YYYY-MM.csv`), optional model file (`/model/`). The device boots and runs without an SD card — live view still works; capture/history features show a clear "no SD card" state instead of failing silently.
- **NVS**: WiFi credentials, IP config, all §5 settings, favorite-species list.
- SD writes are sequenced through a single writer task (camera capture, log append and HTTP static serving must not interleave mid-file).
- On a 1-bit SDMMC bus the board map may name a **DAT3 pull-up pin**, which the mount pulls high before probing. The card latches DAT3 at reset to choose SD or SPI mode, and the IDF driver only pulls up CMD and D0 below a 4-bit width, so a floating DAT3 can leave a perfectly good card refusing `send_op_cond` — which presents as an empty slot, not as an error. The pin is named per board because it is only safe to drive where the wiring says so.

---

## 8. OTA Updates

- Web-based OTA (OTA Update tab): upload a `.bin` from the browser, RemoteStart-style, with bounded `httpd_req_recv()` retries (v1.5 lesson) and dual OTA partitions with rollback on boot failure.
- Release binaries are produced by CI (§9) so open-source users never need an ESP-IDF toolchain to stay current: download `birdbox-vX.Y.Z.bin` from GitHub Releases → OTA tab → upload.

---

## 9. GitHub, CI & Open-Source Deliverables

- **Repository:** GitHub, public when ready for release. Layout:
  ```
  BirdBox/
  ├── FSD_BirdBox.md          # this document — the clean current specification
  ├── FSD_BirdBox_CHANGELOG.md # the change record, one entry per functional change
  ├── README.md               # user-facing: hardware list, wiring, flash & first-boot walkthrough
  ├── LICENSE                 # MIT
  ├── main/                   # ESP-IDF app (C)
  ├── components/             # esp32-camera, esp-dl model runner
  ├── docs/                   # enclosure/mounting notes, model-swap guide
  └── .github/workflows/      # CI
  ```
- **CI (GitHub Actions):** every push builds with the pinned ESP-IDF version for both targets (esp32s3 primary, esp32 fallback); a `v*` tag creates a GitHub Release with the built `.bin` files and auto-generated per-target version lines (RemoteStart v1.16 pattern).
- **Versioning:** `FIRMWARE_VERSION` in `version.h`, semver; FSD changelog is the change record.
- **Open-source posture:** issues/PRs welcome; docs must be good enough that a stranger with the listed hardware succeeds without asking. Community label corrections (§3.2) may be pooled via voluntary GitHub issue uploads to retrain the regional models — no telemetry, ever, and nothing leaves the device automatically.

---

## 10. Development Environment

- ESP-IDF v6.x on Windows (`idf.py build/flash/monitor`), same toolchain installation as RemoteStart/esp32_clock.
- `idf.py set-target esp32s3` for the primary board.
- `sdkconfig.defaults` per target committed to the repo (PSRAM enabled, camera, FATFS long filenames, httpd tuning per §4.7).

---

## 11. Non-Goals (v1) & Future Candidates

**Out of scope for v1:**
- Cloud services, accounts, or any off-LAN access built into the device (§3.3).
- Audio capture / birdsong identification.
- Battery/solar power management and deep-sleep operation (PIR wake groundwork is laid, §3.1).
- Video with audio; H.264 encoding.
- Web UI authentication.

**Likely v2 candidates:** basic-auth for the web UI, birdsong ID (I2S microphone), battery/deep-sleep mode, MQTT publishing for Home Assistant, multi-camera aggregation page.

**Deferred species-ID options (analysed with DetectEnhance2 / v1.33, deliberately not implemented — revisit with field data from the new `roi`/`top3` visit-log columns):**
- *Score-averaging ensemble across best-of-N frames:* instead of keeping the single most confident frame's result, accumulate the per-class scores across all N zoomed frames and decide on the average (standard ensembling; 965 int sums, trivial memory). More robust to one lucky/noisy softmax, but it changes decision semantics (confidence values shift relative to the user's threshold) and needs validation against real visit data before replacing max-pick.
- *Bilinear resize of the 224×224 model input:* the crop currently uses nearest-neighbor, which is blocky when the ROI upscales. Largely mitigated by the ROI-aware decode scale (v1.33); worth ~a few % accuracy at most, cheap to add if inference quality still lags.
- *Adaptive per-pixel motion threshold:* `PIX_DIFF_THR` is fixed at 25 gray levels; sensor noise rises at dusk/dawn, so a noise floor estimated from quiet frames (e.g. `thr = max(15, 3×mean|cur−bg|)`) would keep trigger behaviour consistent across lighting. No field evidence of a problem yet — implement only if false triggers/misses correlate with light level.

---

## 12. Acceptance Criteria (v1)

**Status: all seven criteria below met and verified live on the reference unit as of firmware 0.18.0 / FSD v1.21 — v1 is functionally complete.** Ongoing work past this point (region filter, resolution/contrast, Gallery cleanup tools, etc.) refines v1 rather than closing a gap in it.

1. Fresh flash → `BirdBox-Config` portal → scanned SSID selected → device on home WiFi in under 3 minutes, no toolchain needed.
2. A bird landing at the feeder produces exactly one visit event with ≥ 1 sharp full-res JPEG on SD and a visit-log row.
3. A common regional species in good light is correctly top-1 identified ≥ 70 % of the time; low-confidence events say "Unidentified bird", never a confident wrong guess above threshold.
4. Live stream viewable in a browser while captures continue to be recorded.
5. Stats tab correctly aggregates a week of real events by day, species and hour.
6. OTA from GitHub Release binary succeeds and survives a mid-upload abort without wedging the web server.
7. Device runs ≥ 7 days unattended with no reboot, no heap-low-water regression, and survives router reboots (indefinite reconnect).

---

## 13. Home Assistant Integration

The box reports its own health to Home Assistant over **MQTT**, so a deployed
unit can be watched from the same dashboard as the rest of the house instead of
by opening its web UI. Off by default; nothing is published, no client is
created and no socket is opened until it is enabled.

**Transport.** Plain MQTT to a broker on the LAN (the Mosquitto add-on), with
broker address, port (default 1883), username and password configured in the
UI. A blank username means an anonymous broker. This is a LAN-local
integration and carries no TLS, the same posture as the rest of the web UI.

**Discovery.** The box publishes Home Assistant **MQTT Discovery** configs, so
HA creates every entity by itself and no YAML is written by hand. Discovery
messages are **retained** (HA rebuilds the entities after a restart without the
box being present); the state message is **not** retained, so a restarting HA is
never handed a stale reading as if it were current. All entities carry one
device block and collapse into a single **BirdBox** device whose configuration
URL links back to the box's own web UI.

**Update model.** One state topic for the whole box, not one per entity: every
entity's discovery config points at the same topic and selects its own field
with a `value_template`. Nineteen entities therefore cost **one** publish every
**60 seconds** rather than nineteen.

**Availability** is a **last will**, not a goodbye message. A bird box loses
power or WiFi far more often than it shuts down cleanly, so "offline" comes
from the broker noticing the box stopped answering.

**Published set** — diagnostics: SoC temperature, WiFi signal, free heap, free
internal RAM, largest internal block, free PSRAM, uptime, SD free, SD used,
WiFi reconnects, firmware version, IP address, SD-card and camera health.
Operational: capture events, motion triggers, last species, last confidence,
and a motion binary sensor. Night sleep (§14) reports its state, the reason an
enabled box is still awake, whether the sensor is powered and how long it has
been asleep, so dusk and dawn transitions are visible and alertable in HA with
history rather than only by polling the box. Camera health is reported as a
`problem` binary sensor (auto-recovery gave up, needs a power cycle) plus a
recovery counter — a climbing count is the early warning that shows up long
before a hard fault, and nightly sleep/wake cycling is new stress on that path.
Motion cluster telemetry (§3.1) is published too — last cluster size, the
current cap, the oversized-rejection count and the largest rejected cluster —
because the useful diagnostic is a TREND: rejections climbing while triggers
stay flat means the cap is too low for the mount and the box is discarding real
birds as wind.
A camera that is deliberately asleep is NOT reported as a fault, or HA would
raise "camera disconnected" every night. An unavailable on-die temperature sensor is
**omitted** from the message rather than published as its `-1000` sentinel, so
HA shows "unknown" instead of drawing a cliff through the history graph.

**Forgetting credentials.** The broker password is write-only in the settings API and present-only on save (blank means "keep the stored one"), so clearing the **broker address** and saving is what forgets the stored username and password — otherwise a stored secret would be unremovable short of a factory reset.

**Failure posture.** The client starts **last** in the boot sequence, after the
image has already cast its OTA rollback vote (§8), so an unreachable broker or a
bad credential can never cost an otherwise-good image its validation. Losing the
broker degrades to "no telemetry" and never affects detection, capture or
classification. Because MQTT otherwise fails silently — a wrong password simply
means entities never appear — the Settings tab exposes the live client state
(connected, updates sent, or the specific error) behind a **Check connection**
button.

---

## 14. Night Sleep

There are no birds at night, so detection, capture, identification, the iNat
calls and the illuminator are all waste after dark — along with the heat they
make. The box therefore stops working when its own view goes dark, and resumes
by itself at dawn. Off by default.

**The trigger is what the camera sees, not a clock.** It reuses the ambient
luma reading `motion.c` already computes from every detect frame, with the same
hysteresis the illuminator follows. No location, no clock and no sunrise table
are required, so a shaded or north-facing site behaves correctly and nothing
needs re-tuning as the seasons move.

**Sleeping and waking are deliberately asymmetric, and that is the whole
design.** Going to sleep is driven by a continuous, free measurement. Waking
cannot be: pausing detection stops the frame grabs that produce that
measurement, and powering the sensor down makes it absolute — the box has
switched off the eye that would see dawn. Waking is therefore a timed **probe**:
power the camera, discard the AEC warm-up frames, take one reading, then either
resume or go back to sleep. The probe costs about a second of camera time per
interval (default 10 minutes, so well under 1 % duty cycle), and dawn takes
roughly half an hour, so it is caught comfortably inside the useful window.

**Three levels**, because mains and battery want opposite answers:

| Level | Behaviour |
|---|---|
| **Stay online** (default) | Never sleeps. |
| **Pause detection, camera off** | Detection stops and the sensor is powered down. The web UI, live view state, OTA and Home Assistant all stay up, so the box is always reachable. |
| **Deep sleep** | ESP32 deep sleep between probes. Saves far more, but the box leaves the LAN between wake windows — only worth it on battery or solar. |

On this SoC a deep-sleep wake is a **reset**, not a resume, so every probe wake
is an ordinary boot that brings WiFi, the web server and Home Assistant up by
itself. The box is therefore reachable for a window on each wake and only
returns to sleep if it is still dark. The cost is that a wake pays a full
bringup — NTP resync, SD remount, camera init.

**Safeguards.** The box never sleeps out from under a live stream viewer or an
OTA in flight. A grab wedged in the driver cancels the sleep rather than
deinitialising underneath it. A failed decode counts as *inconclusive*, not as
darkness, so a transient error cannot extend the night. A manual **Wake now**
holds off re-sleeping, so inspecting the box after dark does not fight the
scheduler. And because a blocked view — snow, a leaf, a bird roosting on the
lens — reads dark forever, the box force-wakes after **14 hours** asleep and
stays awake for a snooze period, so a stuck reading self-heals daily instead of
costing a season. That backstop is a maximum sleep duration rather than an
almanac sanity-check on purpose: at the latitudes this project runs at, real
polar nights make "the sun must be up by now" simply false for weeks.

**A camera re-init resets the sensor, and cached settings must notice.** Every
initialisation — boot, watchdog recovery and night wake alike — returns the
sensor to a known baseline, notably with fast shutter off.
`camera_init_generation()` increments on each one, and any module caching sensor
register state re-applies on a change. Without that, a cache describes a sensor
that no longer exists: the box metered the night with full auto exposure while
believing it had a short one, read the dark as bright, and inverted its own
night schedule.

**The wake probe settles until its reading converges**, not for a fixed time. A
freshly woken sensor is still hunting, and a fixed ~1 s under-reads: a dawn
probe measured 70 where the settled detector measured 105 on the same scene.

**Pausing must never blind the sensor that ends the pause.** Night sleep owns its own pause flag, separate from the maintenance toggle, so two independent reasons to stop detecting cannot overwrite each other. A pause must also not blind the reading, so a maintenance-paused loop keeps taking one ambient sample per pass. A NIGHT-paused loop does not: there the night task owns the measurement through its wake probe, and exactly one task may decode a frame at a time, because the decode buffers are shared. Entering a night pause also switches the illuminator off, since the only code that drives it is the paused loop. Without that, a pause freezes the ambient reading and the box can never notice the daylight that would end it. The awake state is re-asserted on every scheduler pass, so a disagreement between the pause flag, camera power and this module's own state corrects itself within one tick rather than latching.

**Interaction with the camera watchdog.** A sleeping sensor is invisible to the
watchdog rather than something it keeps trying to recover: the watchdog already
ignores a camera that reports unavailable and only acts when consumers are
actively grabbing. On wake the frame-heartbeat is re-seeded, so a night-old
timestamp cannot read as a stall.
