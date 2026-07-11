#!/usr/bin/env python3
"""BirdBox Nordic retrain — transfer-learn a MobileNetV2 224 classifier on the
locally collected, human-confirmed captures and export a device-ready int8
TFLite model + index-aligned label file (FSD §3.2.1 / §3.2.2, Phase B).

This is the off-device half of the closed loop. It reads the images you have
hand-labeled on the BirdBox (exported by `export-labels.ps1` into `dataset/`,
plus any curated folders like `lavskrike/candidate/`), fine-tunes a small
classifier, and emits:

    <OUT_NAME>.tflite   full-int8, 1x224x224x3, MobileNetV2 op-set
    <OUT_NAME>.txt      one label per line, "Latin (Common)" or "background"

then drops onto the SD `/model` dir or `POST /model/upload` — no reflash
(docs/MODEL.md). Nothing here runs on the device; a Colab GPU or a plain CPU
is enough for a small class set.

Device contract this script MUST satisfy (verified against main/classify.cpp):
  * Input: the firmware feeds `pixel ^ 0x80` (== pixel-128) straight into the
    int8 input tensor and does NOT apply the tensor's scale/zero-point
    (classify.cpp:420-427). So the model's input must be int8 with
    zero_point == 0 and be trained on x = (pixel-128)/128  ∈ [-1, 1).
  * Ops: the firmware registers exactly six builtins — CONV_2D,
    DEPTHWISE_CONV_2D, ADD, AVERAGE_POOL_2D, FULLY_CONNECTED, SOFTMAX. The
    classifier head therefore uses AveragePooling2D + a 1x1 Conv (not
    GlobalAveragePooling+Dense, which lowers to MEAN/RESHAPE — unregistered).
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
import glob
import os
import sys

# ── Config ──────────────────────────────────────────────────────────────────
ROOT = os.path.dirname(os.path.abspath(__file__))            # training-data/
IMG_SIZE = 224
OUT_NAME = "nordic-v1"
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
# image sources relative to this script; several dirs merge into one class,
# which is how the pre-0.48.0 free-typed "Lavskrike/" folder and the new
# binomial-keyed "Perisoreus_infaustus/" folder (and the 126 curated real
# captures in lavskrike/candidate/) collapse into a single Siberian Jay class.
# Set latin="" and common="background" for the no-bird guard class.
CLASSES = [
    {"latin": "Pyrrhula pyrrhula", "common": "Eurasian Bullfinch",
     "dirs": ["dataset/Pyrrhula_pyrrhula"]},
    {"latin": "Perisoreus infaustus", "common": "Siberian Jay",
     "dirs": ["dataset/Perisoreus_infaustus", "dataset/Lavskrike",
              "lavskrike/candidate"]},
    # Uncomment once you have confirmed-no-bird frames exported (a real
    # deployment needs this guard; a pipeline proof can omit it):
    # {"latin": "", "common": "background", "dirs": ["dataset/no_bird"]},
]

ALLOWED_OPS = {"CONV_2D", "DEPTHWISE_CONV_2D", "ADD",
               "AVERAGE_POOL_2D", "FULLY_CONNECTED", "SOFTMAX"}
IMG_EXTS = (".jpg", ".jpeg", ".png")


# ── Data ────────────────────────────────────────────────────────────────────
def list_images():
    """Return (paths, labels, class_names) from the CLASSES config."""
    paths, labels, names = [], [], []
    for idx, c in enumerate(CLASSES):
        names.append(_label_line(c))
        seen = set()
        n = 0
        for d in c["dirs"]:
            base = os.path.join(ROOT, d)
            if not os.path.isdir(base):
                continue
            for f in glob.glob(os.path.join(base, "**", "*"), recursive=True):
                if not f.lower().endswith(IMG_EXTS):
                    continue
                ap = os.path.abspath(f)
                if ap in seen:
                    continue
                seen.add(ap)
                paths.append(ap)
                labels.append(idx)
                n += 1
        print(f"  [{idx}] {names[-1]:<38} {n} images")
        if n == 0:
            print(f"      !! no images for this class — check its `dirs`")
    return paths, labels, names


def _label_line(c) -> str:
    """The exact string written to the .txt for this class (device format)."""
    latin, common = c["latin"].strip(), c["common"].strip()
    if not latin:                       # no-bird guard class
        return "background"
    return f"{latin} ({common})"


def split(paths, labels, n_classes):
    """Stratified per-class train/val split (deterministic)."""
    import random
    rng = random.Random(SEED)
    by_cls = {i: [] for i in range(n_classes)}
    for p, y in zip(paths, labels):
        by_cls[y].append(p)
    tr_p, tr_y, va_p, va_y = [], [], [], []
    for y, ps in by_cls.items():
        rng.shuffle(ps)
        k = int(round(len(ps) * VAL_FRAC)) if len(ps) >= 5 else 0
        for p in ps[:k]:
            va_p.append(p); va_y.append(y)
        for p in ps[k:]:
            tr_p.append(p); tr_y.append(y)
    return (tr_p, tr_y), (va_p, va_y)


def preprocess(pixel):                  # tf tensor uint8 -> float [-1,1)
    import tensorflow as tf
    return (tf.cast(pixel, tf.float32) - 128.0) / 128.0   # device contract


def make_ds(paths, labels, training):
    import tensorflow as tf

    def load(path, y):
        img = tf.io.decode_image(tf.io.read_file(path), channels=3,
                                 expand_animations=False)
        img = tf.image.resize(img, (IMG_SIZE, IMG_SIZE))
        return img, y

    def augment(img, y):
        # Cheap, dependency-free approximations of this camera's real capture
        # conditions (FSD §3.2.1 step 3). Motion blur is left as a TODO — it
        # needs a small depthwise-conv kernel; add once stock images arrive.
        img = tf.image.random_flip_left_right(img)
        img = tf.image.random_brightness(img, 0.15)
        img = tf.image.random_contrast(img, 0.85, 1.15)
        img = tf.image.random_saturation(img, 0.85, 1.15)
        # Random-resized crop ~ edge-clipping / distance variation:
        scale = tf.random.uniform([], 0.75, 1.0)
        crop = tf.cast(scale * IMG_SIZE, tf.int32)
        img = tf.image.random_crop(img, (crop, crop, 3))
        img = tf.image.resize(img, (IMG_SIZE, IMG_SIZE))
        return img, y

    ds = tf.data.Dataset.from_tensor_slices((paths, labels))
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

    backbone = tf.keras.applications.MobileNetV2(
        input_shape=(IMG_SIZE, IMG_SIZE, 3), alpha=1.0,
        include_top=False, weights="imagenet")
    backbone.trainable = False

    # Head kept inside the 6 registered ops: AveragePooling2D collapses the
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


# ── Export ──────────────────────────────────────────────────────────────────
def export_tflite(model, rep_paths, out_path):
    import tensorflow as tf

    def rep_gen():
        # Representative set drives activation ranges; reuse training images in
        # the exact device preprocessing so the input quant matches the contract.
        for p in rep_paths[:200]:
            raw = tf.io.decode_image(tf.io.read_file(p), channels=3,
                                     expand_animations=False)
            img = tf.image.resize(raw, (IMG_SIZE, IMG_SIZE))
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
    """Check the produced model against the device contract. Returns True/ok."""
    import tensorflow as tf
    ok = True
    print(f"\n── Verifying {os.path.basename(tflite_path)} against device contract ──")

    # Op set (parse the flatbuffer via the schema bundled in tools/).
    ops = _op_set(tflite_path)
    extra = ops - ALLOWED_OPS
    print(f"  ops: {sorted(ops)}")
    if extra:
        ok = False
        print(f"  !! FAIL: ops outside the firmware's registered six: {sorted(extra)}")
        print("     Fix the head, or register these in main/classify.cpp's resolver.")
    else:
        print("  ok: op set within the registered six")

    # Input/output quantization.
    interp = tf.lite.Interpreter(model_path=tflite_path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    iscale, izp = inp["quantization"]
    print(f"  input : dtype={inp['dtype'].__name__} shape={list(inp['shape'])} "
          f"scale={iscale:.6f} zero_point={izp}")
    print(f"  output: dtype={out['dtype'].__name__} shape={list(out['shape'])} "
          f"scale={out['quantization'][0]:.6f} zero_point={out['quantization'][1]}")
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
    return ok


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
        sys.exit(0 if verify(args.verify_only) else 1)

    import tensorflow as tf
    print(f"TensorFlow {tf.__version__}")
    print("Classes / image counts:")
    paths, labels, names = list_images()
    n_classes = len(CLASSES)
    if len(paths) < n_classes * 2:
        sys.exit("Not enough images to train — collect/confirm more first.")

    (tr_p, tr_y), (va_p, va_y) = split(paths, labels, n_classes)
    print(f"\nSplit: {len(tr_p)} train / {len(va_p)} val")

    # Balanced class weights (few-shot classes are heavily outnumbered).
    counts = [tr_y.count(i) for i in range(n_classes)]
    total = sum(counts)
    class_weight = {i: (total / (n_classes * c)) if c else 0.0
                    for i, c in enumerate(counts)}
    print("class_weight:", {i: round(w, 2) for i, w in class_weight.items()})

    tr = make_ds(tr_p, tr_y, training=True)
    va = make_ds(va_p, va_y, training=False) if va_p else None

    model, backbone = build_model(n_classes)
    print("\nPhase 1: training head (backbone frozen)")
    compile_fit(model, tr, va, class_weight, HEAD_EPOCHS, HEAD_LR)

    if FT_EPOCHS > 0:
        print(f"\nPhase 2: fine-tuning top {FT_UNFREEZE} backbone layers")
        backbone.trainable = True
        for layer in backbone.layers[:-FT_UNFREEZE]:
            layer.trainable = False
        compile_fit(model, tr, va, class_weight, FT_EPOCHS, FT_LR)

    tflite_path = os.path.join(ROOT, args.out + ".tflite")
    txt_path = os.path.join(ROOT, args.out + ".txt")
    print(f"\nExporting int8 TFLite -> {tflite_path}")
    export_tflite(model, tr_p, tflite_path)
    write_labels(names, txt_path)
    print(f"Wrote labels -> {txt_path}")
    print(f"Model size: {os.path.getsize(tflite_path) / 1e6:.2f} MB")

    ok = verify(tflite_path)
    print("\nNext: upload to the device (see docs/MODEL.md):")
    print(f'  curl -X POST --data-binary @{args.out}.tflite '
          f'"http://192.168.1.111/model/upload?name={args.out}.tflite"')
    print(f'  curl -X POST --data-binary @{args.out}.txt '
          f'"http://192.168.1.111/model/upload?name={args.out}.txt"')
    print("  then Settings -> Region / species model -> select it, and reboot.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
