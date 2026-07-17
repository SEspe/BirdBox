#!/usr/bin/env python3
"""BirdBox Nordic retrain — transfer-learn a MobileNetV2 224 classifier on the
locally collected, human-confirmed captures and export a device-ready int8
TFLite model + index-aligned label file (FSD §3.2.1 / §3.2.2, Phase B).

This is the off-device half of the closed loop. It reads the images you have
hand-labeled on the BirdBox (exported by `export-labels.ps1` into `dataset/`
with a per-row `roi` column in `dataset/labels.csv`), fine-tunes a small
classifier, and emits:

    <OUT_NAME>.tflite   full-int8, 1x224x224x3, MobileNetV2 op-set
    <OUT_NAME>.txt      one label per line, "Latin (Common)" or "background"

then drops onto the SD `/model` dir or `POST /model/upload` — no reflash
(docs/MODEL.md). Nothing here runs on the device; a Colab GPU or a plain CPU
is enough for a small class set.

ROI-crop training (v0.4+):
  The classifier is trained on the SAME square crop the device feeds at
  inference when `detect_zoom` is on — the motion ROI (bird box), expanded to a
  square and resized to 224 with NO stretch. Each image's box is read from the
  `roi` column of `dataset/labels.csv` ("x0-y0-x1-y1", fractions of the frame),
  and `roi_square()` here mirrors `classify.cpp decode_to_input` exactly
  (side = max(box_w, box_h), capped at min(frame_w, frame_h), centred on the box
  centre, clamped in-frame). Only rows that HAVE an roi are used — the operator
  chose tight-crop training, and an image with no logged box can't be cropped to
  one. Consequence: **the device must serve this model with `detect_zoom` ON**,
  or it would feed whole frames to a crop-trained model (train/serve skew).

Device contract this script MUST satisfy (verified against main/classify.cpp):
  * Input: the firmware feeds `pixel ^ 0x80` (== pixel-128) straight into the
    int8 input tensor and does NOT apply the tensor's scale/zero-point
    (classify.cpp:420-427). So the model's input must be int8 with
    zero_point == 0 and be trained on x = (pixel-128)/128  ∈ [-1, 1).
  * Ops: the firmware registers seven builtins — CONV_2D, DEPTHWISE_CONV_2D,
    ADD, AVERAGE_POOL_2D, FULLY_CONNECTED, SOFTMAX, PAD. The classifier head
    therefore uses AveragePooling2D + a 1x1 Conv (not GlobalAveragePooling+
    Dense, which lowers to MEAN/RESHAPE — unregistered). PAD is emitted by
    Keras MobileNetV2's ZeroPadding2D and is registered for exactly this path.
  * Output: read dynamically (scale/zero_point), so it is unconstrained.
  * Labels: index-aligned .txt, "Scientific name (Common Name)" (localized on
    device via species_i18n.c by the binomial); a literal `background` line is
    the no-bird guard class (docs/MODEL.md:79-82).

The final step VERIFIES the produced .tflite against the input/op contract and
refuses to declare success if it drifts — so a bad export is caught here, on
the PC, not as a silent AllocateTensors failure on the ESP32.

Usage:
    pip install -r requirements.txt
    python train.py                 # trains + exports to ./<OUT_NAME>.{tflite,txt}
    python train.py --verify-only nordic-v1.tflite   # just re-run the checks
"""
from __future__ import annotations
import argparse
import csv
import os
import sys

# TFLite full-int8 conversion is unreliable under Keras 3 (TF>=2.16 default):
# the MLIR converter aborts with `LLVM ERROR: Failed to infer result type(s)` /
# `missing attribute 'value'` on a plain Conv ReadVariableOp. Route tf.keras to
# the classic Keras 2 implementation, whose from_keras_model int8 path is solid.
# Must be set BEFORE TensorFlow is imported — it is, since tf is imported lazily
# inside the functions below. Requires the `tf_keras` package (requirements.txt).
os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")

# Windows consoles default to cp1252, which can't encode the box-drawing / arrow
# glyphs used in the progress prints — force UTF-8 so output never crashes.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ── Config ──────────────────────────────────────────────────────────────────
ROOT = os.path.dirname(os.path.abspath(__file__))            # training-data/
DATASET = os.path.join(ROOT, "dataset")
LABELS_CSV = os.path.join(DATASET, "labels.csv")
IMG_SIZE = 224
LINEAGE = "nordic"          # model family; the stock Coral model is "inat"

# Model version scheme (MAJOR.MINOR), keyed on DATA PROVENANCE, not accuracy:
#   0.x  pre-baseline experiments — include local BirdBox captures, prove the
#        pipeline. 0.4 is the first ROI-crop, 4-species+background build.
#   1.0  first proper baseline trained PURELY from external data (iNaturalist/
#        GBIF stock) — the reference, no device images.
#   1.x  1.0 + x rounds of local BirdBox captures mixed in (domain adaptation).
#   2.0  breaking change — class set added/removed (label indices shift) or a
#        different architecture/input.
# Bump MODEL_VERSION per retrain and record it in the registry (README).
MODEL_VERSION = "0.7"
# Where THIS build's training images come from: "external" (stock only),
# "local" (BirdBox captures only), or "mixed". Recorded in the manifest so a
# model's provenance is never guessed later. A 1.0 must be "external".
# NB: split() now routes external/un-timestamped stock images TRAIN-ONLY, gated
# by the BIRDBOX_EXCLUDE_EXTERNAL toggle (added while evaluating GBIF/stock data,
# which did not improve accuracy on real captures). With a local-only labels.csv
# this is a no-op for the target classes.
DATA_PROVENANCE = "local"

# Backbone weight initialisation: "imagenet" (generic objects, the historical
# default) or "inat" (Google AIY birds_V1 MobileNetV2 — bird-specific features
# across ~965 species, a better start for this task). "inat" transplants only
# *weights*, not stock training images, so it does NOT reintroduce the GBIF
# domain-gap finding (see inat_backbone.py). Override with BIRDBOX_BACKBONE.
BACKBONE_INIT = os.environ.get("BIRDBOX_BACKBONE", "imagenet")

OUT_NAME = f"{LINEAGE}-v{MODEL_VERSION}"
VAL_FRAC = 0.2
SEED = 1337

# Phase 1: train the head with the backbone frozen. Phase 2: unfreeze the top
# of the backbone and fine-tune at a low LR. Keep phase 2 short on tiny data —
# it overfits fast (few-hundred-image regime).
HEAD_EPOCHS = 20
FT_EPOCHS = 8
FT_UNFREEZE = 30            # last N backbone layers to unfreeze in phase 2
BATCH = 16
HEAD_LR = 1e-3
FT_LR = 1e-5

# Each class = one output index (order here == order in the .txt). `dirs` are
# the top-level `dataset/<dir>/` folders (as written in the labels.csv relpath)
# that merge into this class. Set latin="" and common="background" for the
# no-bird guard class. The v0.48.0 free-typed "Lavskrike/" folder still folds
# into the binomial-keyed Siberian Jay class.
CLASSES = [
    {"latin": "Pyrrhula pyrrhula", "common": "Eurasian Bullfinch",
     "dirs": ["Pyrrhula_pyrrhula"]},
    {"latin": "Perisoreus infaustus", "common": "Siberian Jay",
     "dirs": ["Perisoreus_infaustus", "Lavskrike"]},
    {"latin": "Fringilla coelebs", "common": "Common Chaffinch",
     "dirs": ["Fringilla_coelebs"]},
    {"latin": "Pica pica", "common": "Eurasian Magpie",
     "dirs": ["Pica_pica"]},
    {"latin": "Garrulus glandarius", "common": "Eurasian Jay",
     "dirs": ["Garrulus_glandarius"]},   # Nøtteskrike (added v0.7)
    # Reject/guard class — the model's "not a target bird" output. Merges the
    # two human reject buckets (FSD v1.76): no_bird (empty frames) + other
    # (present but non-bird subjects, e.g. cat/sheep — hard negatives). The
    # `unknown` bucket is deliberately absent (excluded at export: a bird can't
    # be a hard negative). A real deployment needs this guard so the classifier
    # can decline rather than force a species onto every frame.
    {"latin": "", "common": "background",
     "dirs": ["no_bird", "other"]},
]

# The builtins the firmware registers (classify.cpp MicroMutableOpResolver).
# PAD was added for the Keras MobileNetV2 retrain path (its ZeroPadding2D
# before stride-2 convs); FULLY_CONNECTED stays registered for stock models
# though this script's conv head doesn't use it.
ALLOWED_OPS = {"CONV_2D", "DEPTHWISE_CONV_2D", "ADD", "AVERAGE_POOL_2D",
               "FULLY_CONNECTED", "SOFTMAX", "PAD"}


# ── Data ────────────────────────────────────────────────────────────────────
def parse_roi(s):
    """'x0-y0-x1-y1' (fractions) -> (x0,y0,x1,y1) floats, or None if malformed.
    Same shape the device writes/validates (web_server h_set_roi): each in
    [0,1], x1>x0, y1>y0. A row failing this is skipped, not crashed on."""
    parts = s.split("-")
    if len(parts) != 4:
        return None
    try:
        x0, y0, x1, y1 = (float(p) for p in parts)
    except ValueError:
        return None
    if not (0.0 <= x0 < x1 <= 1.0 and 0.0 <= y0 < y1 <= 1.0):
        return None
    return (x0, y0, x1, y1)


def _label_line(c) -> str:
    """The exact string written to the .txt for this class (device format)."""
    latin, common = c["latin"].strip(), c["common"].strip()
    if not latin:                       # no-bird guard class
        return "background"
    return f"{latin} ({common})"


def list_roi_images():
    """Read dataset/labels.csv and return (paths, labels, rois, names, counts)
    for the rows that (a) carry a valid roi and (b) map to a configured class.
    ROI-only: rows without a logged box are skipped — this build trains on tight
    crops, and there's nothing to crop to without a box."""
    names, dir2idx = [], {}
    for idx, c in enumerate(CLASSES):
        names.append(_label_line(c))
        for d in c["dirs"]:
            dir2idx[d] = idx

    if not os.path.isfile(LABELS_CSV):
        sys.exit(f"labels.csv not found at {LABELS_CSV} — run export-labels.ps1 first.")

    paths, labels, rois = [], [], []
    counts = [0] * len(CLASSES)
    n_total = n_noroi = n_badroi = n_nomap = n_missing = 0
    with open(LABELS_CSV, encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            n_total += 1
            rel = (row.get("relpath") or "").strip().replace("\\", "/")
            roi_s = (row.get("roi") or "").strip()
            if not rel:
                continue
            if not roi_s:
                n_noroi += 1
                continue
            topdir = rel.split("/")[0]
            idx = dir2idx.get(topdir)
            if idx is None:
                n_nomap += 1
                continue
            roi = parse_roi(roi_s)
            if roi is None:
                n_badroi += 1
                continue
            ap = os.path.join(DATASET, *rel.split("/"))
            if not os.path.isfile(ap):
                n_missing += 1
                continue
            paths.append(os.path.abspath(ap))
            labels.append(idx)
            rois.append(roi)
            counts[idx] += 1

    print(f"labels.csv: {n_total} rows -> {len(paths)} usable (with roi + mapped class)")
    print(f"  skipped: {n_noroi} no-roi, {n_badroi} bad-roi, "
          f"{n_nomap} unmapped-class, {n_missing} missing-file")
    for idx, c in enumerate(CLASSES):
        print(f"  [{idx}] {names[idx]:<38} {counts[idx]} images")
        if counts[idx] == 0:
            print(f"      !! no ROI images for this class — check its `dirs`")
    return paths, labels, rois, names, counts


# Visit-grouped split params. A "visit" = a burst of frames close in time (one
# bird's stay). We split whole visits — never individual frames — so near-dup
# burst frames can't straddle train/val; that leakage is what inflated v0.2's
# val_acc to a meaningless ~99% while it failed on genuinely fresh captures.
# Each visit is also capped so a few long bursts can't dominate a class.
VISIT_GAP_S   = 90     # gap (s) between consecutive frames that starts a new visit
MAX_PER_VISIT = 10     # burst de-dup: keep at most this many frames per visit

_TS_RE = None
def _frame_time(path):
    """Epoch seconds from a 'YYYY-MM-DD_HH-MM-SS-mmm' basename, or None for names
    that don't match (e.g. curated external images)."""
    global _TS_RE
    import re, datetime
    if _TS_RE is None:
        _TS_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})-(\d{3})")
    m = _TS_RE.search(os.path.basename(path))
    if not m:
        return None
    y, mo, d, H, M, S, ms = (int(g) for g in m.groups())
    return datetime.datetime(y, mo, d, H, M, S, ms * 1000).timestamp()


def _group_visits(paths):
    """Cluster one class's frames into visits by timestamp gap. Un-timestamped
    frames each become their own singleton visit (never merged)."""
    stamped = sorted(((_frame_time(p), p) for p in paths),
                     key=lambda t: (t[0] is None, t[0] or 0.0, t[1]))
    visits, cur, last = [], [], None
    for ts, p in stamped:
        if ts is None:
            if cur: visits.append(cur); cur = []
            visits.append([p]); last = None; continue
        if last is not None and ts - last > VISIT_GAP_S:
            visits.append(cur); cur = []
        cur.append(p); last = ts
    if cur:
        visits.append(cur)
    return visits


def _dedup_visit(v):
    """Down-sample a burst to MAX_PER_VISIT evenly-spaced frames."""
    if len(v) <= MAX_PER_VISIT:
        return v
    idx = sorted({round(i * (len(v) - 1) / (MAX_PER_VISIT - 1))
                  for i in range(MAX_PER_VISIT)})
    return [v[i] for i in idx]


def split(paths, labels, n_classes):
    """Visit-grouped, burst-de-duplicated per-class train/val split (deterministic).

    Whole visits go to train OR val — a burst is never split across both — so the
    val set measures generalization to *unseen visits*, not memorized near-dups
    (the v0.2 leakage). Each visit is first capped at MAX_PER_VISIT frames.
    Returns path lists only; ROIs are re-attached by the caller via path.

    External stock images (un-timestamped filenames, e.g. GBIF/artsobservasjoner
    GUIDs) are TRAIN-ONLY: validation must measure the *birdbox* domain, not stock
    hero shots. Because the val draw is over device visits only, adding stock data
    leaves the val set unchanged — so a with/without-stock comparison is a clean
    A/B on an identical held-out birdbox set. Per-class RNG (SEED+y) keeps each
    class's draw independent, so one class's data volume can't shift another's
    val set. Set BIRDBOX_EXCLUDE_EXTERNAL=1 to drop stock entirely (baseline)."""
    import random
    exclude_ext = os.environ.get("BIRDBOX_EXCLUDE_EXTERNAL") == "1"
    by_cls = {i: [] for i in range(n_classes)}
    for p, y in zip(paths, labels):
        by_cls[y].append(p)
    tr_p, tr_y, va_p, va_y = [], [], [], []
    for y, ps in by_cls.items():
        rng = random.Random(SEED + y)                 # per-class, independent
        dev = [p for p in ps if _frame_time(p) is not None]   # birdbox captures
        ext = [] if exclude_ext else [p for p in ps if _frame_time(p) is None]
        visits = [_dedup_visit(v) for v in _group_visits(dev)]
        rng.shuffle(visits)
        k = int(round(len(visits) * VAL_FRAC)) if len(visits) >= 5 else 0
        va = [p for v in visits[:k] for p in v]
        tr = [p for v in visits[k:] for p in v] + ext         # stock: train-only
        va_p += va; va_y += [y] * len(va)
        tr_p += tr; tr_y += [y] * len(tr)
        print(f"  class {y}: {len(visits)} dev visits + {len(ext)} ext "
              f"-> {len(tr)} train / {len(va)} val frames")
    return (tr_p, tr_y), (va_p, va_y)


def preprocess(pixel):                  # tf tensor uint8 -> float [-1,1)
    import tensorflow as tf
    return (tf.cast(pixel, tf.float32) - 128.0) / 128.0   # device contract


def roi_square(img, roi):
    """Square-expand crop of `img` around the motion ROI, then resize to
    IMG_SIZE. This MUST mirror classify.cpp decode_to_input's geometry: take the
    box (roi = x0,y0,x1,y1 fractions), expand the shorter side so the crop is a
    SQUARE (side = max(box_w, box_h)), cap it at the frame's short dimension,
    centre it on the box centre, clamp it fully in-frame, and resize with NO
    stretch. Training on this exact crop is why the device must serve this model
    with detect_zoom ON — otherwise it feeds whole frames to a crop model."""
    import tensorflow as tf
    shp = tf.shape(img)
    H = tf.cast(shp[0], tf.float32)
    W = tf.cast(shp[1], tf.float32)
    x0, y0, x1, y1 = roi[0], roi[1], roi[2], roi[3]
    rw = (x1 - x0) * W
    rh = (y1 - y0) * H
    cx = (x0 + x1) * 0.5 * W
    cy = (y0 + y1) * 0.5 * H
    side = tf.minimum(tf.maximum(rw, rh), tf.minimum(W, H))    # square, capped
    left = tf.clip_by_value(cx - side * 0.5, 0.0, W - side)
    top  = tf.clip_by_value(cy - side * 0.5, 0.0, H - side)
    # Integerize and guarantee the window stays in-bounds after rounding.
    li = tf.cast(tf.round(left), tf.int32)
    ti = tf.cast(tf.round(top), tf.int32)
    si = tf.cast(tf.round(side), tf.int32)
    si = tf.minimum(si, tf.minimum(shp[1] - li, shp[0] - ti))
    si = tf.maximum(si, 1)
    img = tf.image.crop_to_bounding_box(img, ti, li, si, si)
    return tf.image.resize(img, (IMG_SIZE, IMG_SIZE))


def make_ds(paths, labels, rois, training):
    import tensorflow as tf

    roi_t = tf.constant(rois, dtype=tf.float32)     # [N,4], aligned with paths

    def load(path, y, roi):
        img = tf.io.decode_image(tf.io.read_file(path), channels=3,
                                 expand_animations=False)
        img = roi_square(img, roi)        # match the device (ROI square-expand)
        return img, y

    def augment(img, y):
        # Cheap, dependency-free approximations of this camera's real capture
        # conditions (FSD §3.2.1 step 3). The crop is already tight to the bird,
        # so keep the random-resized crop mild (0.85-1.0) — an aggressive crop
        # would slice the subject out of a box that's already the subject.
        img = tf.image.random_flip_left_right(img)
        img = tf.image.random_brightness(img, 0.15)
        img = tf.image.random_contrast(img, 0.85, 1.15)
        img = tf.image.random_saturation(img, 0.85, 1.15)
        scale = tf.random.uniform([], 0.85, 1.0)
        crop = tf.cast(scale * IMG_SIZE, tf.int32)
        img = tf.image.random_crop(img, (crop, crop, 3))
        img = tf.image.resize(img, (IMG_SIZE, IMG_SIZE))
        return img, y

    ds = tf.data.Dataset.from_tensor_slices((paths, labels, roi_t))
    if training:
        ds = ds.shuffle(max(len(paths), 1), seed=SEED, reshuffle_each_iteration=True)
    ds = ds.map(load, num_parallel_calls=tf.data.AUTOTUNE)
    if training:
        ds = ds.map(augment, num_parallel_calls=tf.data.AUTOTUNE)
    ds = ds.map(lambda x, y: (preprocess(x), tf.reshape(y, [1, 1])),
                num_parallel_calls=tf.data.AUTOTUNE)
    ds = ds.batch(BATCH).prefetch(tf.data.AUTOTUNE)
    return ds


# ── Model ───────────────────────────────────────────────────────────────────
def build_model(n_classes):
    import tensorflow as tf
    from tensorflow.keras import layers, Model

    if BACKBONE_INIT == "inat":
        # iNat bird backbone: build empty, then transplant the AIY birds_V1
        # weights. Same Keras MobileNetV2 object as the ImageNet path, so the
        # int8 export + device-contract verify below are unchanged.
        import inat_backbone
        backbone = tf.keras.applications.MobileNetV2(
            input_shape=(IMG_SIZE, IMG_SIZE, 3), alpha=1.0,
            include_top=False, weights=None)
        inat_backbone.load_into(backbone)
    else:
        backbone = tf.keras.applications.MobileNetV2(
            input_shape=(IMG_SIZE, IMG_SIZE, 3), alpha=1.0,
            include_top=False, weights="imagenet")
    backbone.trainable = False

    # Head kept inside the registered ops: AveragePooling2D collapses the
    # HxW feature map (MEAN-free), a 1x1 Conv is the classifier (== the Coral
    # inat model's fully-convolutional head), Softmax over the channel axis.
    # Output stays [b,1,1,C]; the firmware reads C contiguous int8 values.
    h, w = backbone.output_shape[1:3]
    x = layers.AveragePooling2D(pool_size=(h, w), name="avgpool")(backbone.output)
    x = layers.Dropout(0.2)(x)
    x = layers.Conv2D(n_classes, 1, name="logits")(x)      # [b,1,1,C]
    out = layers.Softmax(axis=-1, name="probs")(x)         # [b,1,1,C]
    return Model(backbone.input, out), backbone


def compile_fit(model, tr, va, class_weight, epochs, lr):
    import tensorflow as tf
    model.compile(
        optimizer=tf.keras.optimizers.Adam(lr),
        loss=tf.keras.losses.SparseCategoricalCrossentropy(),
        metrics=[tf.keras.metrics.SparseCategoricalAccuracy(name="acc")])
    return model.fit(tr, validation_data=va if va else None,
                     epochs=epochs, class_weight=class_weight, verbose=2)


def confusion(model, va_p, va_y, va_rois, names):
    """Human-truth vs model-pred confusion matrix on the held-out val visits —
    this is the 'check the new model against my human classification' step. The
    val ds is unshuffled, so predictions align 1:1 with va_y."""
    import numpy as np
    ds = make_ds(va_p, va_y, va_rois, training=False)
    probs = model.predict(ds, verbose=0)
    pred = probs.reshape(len(va_y), -1).argmax(1)
    n = len(names)
    cm = np.zeros((n, n), dtype=int)
    for t, p in zip(va_y, pred):
        cm[t][p] += 1
    print("\nConfusion matrix — rows = human truth, cols = model prediction (val set):")
    print(f"{'':<30}" + "".join(f"{i:>7}" for i in range(n)) + "   recall")
    for i in range(n):
        tot = cm[i].sum()
        rec = cm[i][i] / tot if tot else 0.0
        cells = "".join(f"{v:>7}" for v in cm[i])
        print(f"[{i}] {names[i][:26]:<26}{cells}  {rec*100:5.1f}%")
    # Per-class precision (how trustworthy a given predicted label is).
    print("precision:" + " " * 20 +
          "".join(f"{(cm[:,j][j]/cm[:,j].sum()*100 if cm[:,j].sum() else 0):>6.0f}%"
                  for j in range(n)))
    overall = np.trace(cm) / cm.sum() if cm.sum() else 0.0
    print(f"Overall val accuracy: {overall*100:.1f}%  (n={int(cm.sum())})")
    return {"matrix": cm.tolist(),
            "overall_acc": float(overall),
            "val_n": int(cm.sum())}


# ── Export ──────────────────────────────────────────────────────────────────
def export_tflite(model, rep_paths, rep_rois, out_path):
    import tensorflow as tf

    def rep_gen():
        # Representative set drives activation ranges; reuse training images in
        # the exact device preprocessing so the input quant matches the contract.
        for p, r in list(zip(rep_paths, rep_rois))[:200]:
            raw = tf.io.decode_image(tf.io.read_file(p), channels=3,
                                     expand_animations=False)
            img = roi_square(raw, tf.constant(r, tf.float32))   # same device geometry
            yield [tf.expand_dims(preprocess(img), 0)]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_gen
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    data = conv.convert()
    with open(out_path, "wb") as f:
        f.write(data)
    return out_path


def write_labels(names, out_path):
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(names) + "\n")


def verify(tflite_path):
    """Check the produced model against the device contract.
    Returns (ok, info) where info captures the op set + I/O quant for the
    manifest, so provenance/contract facts are recorded, not re-derived."""
    import tensorflow as tf
    ok = True
    print(f"\n── Verifying {os.path.basename(tflite_path)} against device contract ──")

    # Op set (parse the flatbuffer via the schema bundled in tools/).
    ops = _op_set(tflite_path)
    extra = ops - ALLOWED_OPS
    print(f"  ops: {sorted(ops)}")
    if extra:
        ok = False
        print(f"  !! FAIL: ops outside the firmware's registered set: {sorted(extra)}")
        print("     Fix the head, or register these in main/classify.cpp's resolver.")
    else:
        print("  ok: op set within the registered set")

    # Input/output quantization.
    interp = tf.lite.Interpreter(model_path=tflite_path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    iscale, izp = inp["quantization"]
    oscale, ozp = out["quantization"]
    print(f"  input : dtype={inp['dtype'].__name__} shape={list(inp['shape'])} "
          f"scale={iscale:.6f} zero_point={izp}")
    print(f"  output: dtype={out['dtype'].__name__} shape={list(out['shape'])} "
          f"scale={oscale:.6f} zero_point={ozp}")
    if inp["dtype"].__name__ != "int8" or list(inp["shape"]) != [1, IMG_SIZE, IMG_SIZE, 3]:
        ok = False
        print("  !! FAIL: input must be int8 1x224x224x3")
    if izp != 0:
        ok = False
        print(f"  !! FAIL: input zero_point is {izp}, but the firmware feeds "
              "pixel-128 assuming zero_point==0 (classify.cpp:427).")
        print("     Either retrain so PTQ yields zp==0, or make classify.cpp read "
              "the input scale/zp like it already does for the output.")
    if out["dtype"].__name__ != "int8":
        ok = False
        print("  !! FAIL: output must be int8 (firmware reads int8 + params)")

    print("  RESULT:", "PASS — safe to upload" if ok else "FAIL — do NOT upload")
    info = {
        "ops": sorted(ops),
        "ops_ok": not extra,
        "input": {"dtype": inp["dtype"].__name__,
                  "shape": [int(x) for x in inp["shape"]],
                  "scale": float(iscale), "zero_point": int(izp)},
        "output": {"dtype": out["dtype"].__name__,
                   "shape": [int(x) for x in out["shape"]],
                   "scale": float(oscale), "zero_point": int(ozp)},
        "contract_pass": bool(ok),
    }
    return ok, info


def write_manifest(out_path, names, counts, split_sizes, verify_info, eval_info):
    """Sidecar JSON recording a model's identity + provenance, so a .tflite on
    an SD card is never an anonymous blob. The device ignores it; it's the
    PC-side registry record (see the versioning scheme in the config)."""
    import datetime
    import json
    try:
        import tensorflow as tf
        tf_ver = tf.__version__
    except Exception:
        tf_ver = None
    manifest = {
        "lineage": LINEAGE,
        "version": MODEL_VERSION,
        "data_provenance": DATA_PROVENANCE,
        "crop": "roi-square-expand (detect_zoom ON required on device)",
        "built": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "tensorflow": tf_ver,
        "input_size": IMG_SIZE,
        "backbone": f"MobileNetV2 alpha=1.0 ({'iNat birds_V1' if BACKBONE_INIT=='inat' else 'ImageNet'})",
        "classes": [{"index": i, "label": n, "train_images": counts[i]}
                    for i, n in enumerate(names)],
        "train_count": split_sizes[0],
        "val_count": split_sizes[1],
        "hparams": {"head_epochs": HEAD_EPOCHS, "ft_epochs": FT_EPOCHS,
                    "ft_unfreeze": FT_UNFREEZE, "batch": BATCH,
                    "head_lr": HEAD_LR, "ft_lr": FT_LR, "val_frac": VAL_FRAC,
                    "seed": SEED},
        "verify": verify_info,
        "eval": eval_info,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    return manifest


def _op_set(tflite_path):
    """Set of builtin op names used by the model."""
    sys.path.insert(0, os.path.join(ROOT, "..", "tools"))
    import schema_py_generated as tfl
    with open(tflite_path, "rb") as f:
        model = tfl.ModelT.InitFromPackedBuf(bytearray(f.read()), 0)
    code_name = {v: k for k, v in vars(tfl.BuiltinOperator).items()
                 if isinstance(v, int)}
    ops = set()
    for oc in model.operatorCodes:
        bc = max(getattr(oc, "builtinCode", 0) or 0,
                 getattr(oc, "deprecatedBuiltinCode", 0) or 0)
        ops.add(code_name.get(bc, f"UNKNOWN({bc})"))
    return ops


# ── Main ────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify-only", metavar="MODEL.tflite",
                    help="skip training; just run the contract checks on a model")
    ap.add_argument("--out", default=OUT_NAME, help="output basename")
    args = ap.parse_args()

    if args.verify_only:
        ok, _ = verify(args.verify_only)
        sys.exit(0 if ok else 1)

    # Provenance guard: a .0 baseline from v1.0 up must be pure external
    # (the whole point of "1.0 = pure from external").
    major, _, minor = MODEL_VERSION.partition(".")
    if int(major) >= 1 and minor in ("0", "") and DATA_PROVENANCE != "external":
        sys.exit(f"Version {MODEL_VERSION} is a baseline (.0) but DATA_PROVENANCE="
                 f"'{DATA_PROVENANCE}'. A vN.0 must be trained purely from external "
                 "data; bump to a .1+ for local/mixed data, or set provenance.")

    import tensorflow as tf
    print(f"TensorFlow {tf.__version__}")
    print(f"Building {LINEAGE}-v{MODEL_VERSION}  (provenance: {DATA_PROVENANCE}, ROI-crop)")
    print("Classes / ROI image counts:")
    paths, labels, rois, names, counts = list_roi_images()
    n_classes = len(CLASSES)
    if len(paths) < n_classes * 2:
        sys.exit("Not enough ROI images to train — backfill/confirm more first.")

    roi_by_path = dict(zip(paths, rois))
    (tr_p, tr_y), (va_p, va_y) = split(paths, labels, n_classes)
    tr_rois = [roi_by_path[p] for p in tr_p]
    va_rois = [roi_by_path[p] for p in va_p]
    print(f"\nSplit: {len(tr_p)} train / {len(va_p)} val")

    # Balanced class weights (few-shot classes are heavily outnumbered).
    tr_counts = [tr_y.count(i) for i in range(n_classes)]
    total = sum(tr_counts)
    class_weight = {i: (total / (n_classes * c)) if c else 0.0
                    for i, c in enumerate(tr_counts)}
    print("class_weight:", {i: round(w, 2) for i, w in class_weight.items()})

    tr = make_ds(tr_p, tr_y, tr_rois, training=True)
    va = make_ds(va_p, va_y, va_rois, training=False) if va_p else None

    model, backbone = build_model(n_classes)
    print("\nPhase 1: training head (backbone frozen)")
    compile_fit(model, tr, va, class_weight, HEAD_EPOCHS, HEAD_LR)

    if FT_EPOCHS > 0:
        print(f"\nPhase 2: fine-tuning top {FT_UNFREEZE} backbone layers")
        backbone.trainable = True
        for layer in backbone.layers[:-FT_UNFREEZE]:
            layer.trainable = False
        compile_fit(model, tr, va, class_weight, FT_EPOCHS, FT_LR)

    eval_info = None
    if va_p:
        eval_info = confusion(model, va_p, va_y, va_rois, names)

    tflite_path = os.path.join(ROOT, args.out + ".tflite")
    txt_path = os.path.join(ROOT, args.out + ".txt")
    json_path = os.path.join(ROOT, args.out + ".json")
    print(f"\nExporting int8 TFLite -> {tflite_path}")
    export_tflite(model, tr_p, tr_rois, tflite_path)
    write_labels(names, txt_path)
    print(f"Wrote labels -> {txt_path}")
    print(f"Model size: {os.path.getsize(tflite_path) / 1e6:.2f} MB")

    ok, info = verify(tflite_path)
    write_manifest(json_path, names, counts, (len(tr_p), len(va_p)), info, eval_info)
    print(f"Wrote manifest -> {json_path}")
    print("\nNext: upload to the device (see docs/MODEL.md):")
    print(f'  curl -X POST --data-binary @{args.out}.tflite '
          f'"http://192.168.1.111/model/upload?name={args.out}.tflite"')
    print(f'  curl -X POST --data-binary @{args.out}.txt '
          f'"http://192.168.1.111/model/upload?name={args.out}.txt"')
    print("  then Settings -> Region / species model -> select it, enable detect_zoom, reboot.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
