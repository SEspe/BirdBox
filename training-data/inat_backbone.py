#!/usr/bin/env python3
"""iNaturalist-backbone initialisation for the BirdBox retrain (FSD §3.2.1).

`train.py` normally initialises its MobileNetV2 backbone from **ImageNet**
(generic objects). This module lets it instead start from the **iNaturalist
bird** weights that Google's AIY `birds_V1` classifier (the same lineage as the
Coral model in docs/MODEL.md) was trained on — a backbone that already encodes
bird-specific features (plumage, beak, posture) across ~965 species. Starting
there and fine-tuning on real BirdBox captures is a strictly better init than
ImageNet for our task.

IMPORTANT — this transplants *weights*, not training *images*. It does NOT
reintroduce the stock-image domain gap that the GBIF A/B ruled out (SESSION_NOTES
2026-07-16): the training set stays 100% real BirdBox frames; only the starting
point of the conv filters changes.

How it works (verified 2026-07-17):
  * The AIY birds_V1 SavedModel is a frozen TF1 graph — a bog-standard
    MobileNetV2 1.0 / 224 (`MobilenetV2/Conv` stem, `expanded_conv[_1..16]`
    blocks, `Conv_1` head). Its 260 weight tensors (52 conv/dw kernels +
    52x4 BN params) map 1:1 onto `tf.keras.applications.MobileNetV2(alpha=1.0,
    include_top=False)`.
  * We parse the weights straight out of the GraphDef `Const` nodes (no
    tensorflow_hub dependency, which doesn't play well with Keras 3 / TF 2.16),
    cache them to an .npz, and assign them into a clean Keras backbone.
  * Faithfulness was checked end-to-end: rebuilt-float-classifier logits vs the
    original graph cosine 0.92-0.98, orig top-1 always within the transplant's
    top-5; the stem reproduces to 8-bit-quantisation precision. The residual is
    just the original's QAT fake-quant, irrelevant to a float fine-tuning init.
  * Because the result is an ordinary Keras MobileNetV2, `train.py`'s int8
    export + 7-op device-contract verification are completely unchanged.

Input-range note: the AIY graph normalises [0,1] -> [-1,1] (x*2-1) before the
backbone; `train.py`'s `preprocess()` feeds (pixel-128)/128 == [-1,1) — the same
range — so the transplanted filters see the distribution they were trained on.
"""
from __future__ import annotations
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
CACHE_NPZ = os.path.join(ROOT, "inat-mnv2-backbone.npz")
SAVEDMODEL_PB = os.path.join(ROOT, ".inat-cache", "saved_model.pb")
# tfhub.dev redirects to a signed Kaggle bundle; ?tf-hub-format=compressed
# returns a .tar.gz of the frozen SavedModel (saved_model.pb + tfhub_module.pb).
HUB_URL = ("https://tfhub.dev/google/aiy/vision/classifier/birds_V1/1"
           "?tf-hub-format=compressed")


def _slim_prefix(kname: str):
    """Map a Keras MobileNetV2 layer name -> (slim scope prefix, kind).

    kind is 'conv' | 'dwconv' | 'bn'. Mirrors the TF-Slim naming the AIY graph
    uses (`MobilenetV2/expanded_conv_<i>/{expand,depthwise,project}/...`)."""
    if kname == "Conv1":       return "MobilenetV2/Conv", "conv"
    if kname == "bn_Conv1":    return "MobilenetV2/Conv/BatchNorm", "bn"
    if kname == "Conv_1":      return "MobilenetV2/Conv_1", "conv"
    if kname == "Conv_1_bn":   return "MobilenetV2/Conv_1/BatchNorm", "bn"
    if kname.startswith("expanded_conv_"):     # Keras block 0: no numeric index
        role = kname[len("expanded_conv_"):]
        bn = role.endswith("_BN"); role = role[:-3] if bn else role
        base = f"MobilenetV2/expanded_conv/{role}"
        if bn: return base + "/BatchNorm", "bn"
        return base, ("dwconv" if role == "depthwise" else "conv")
    if kname.startswith("block_"):             # Keras blocks 1..16
        parts = kname.split("_")
        i, rest = parts[1], "_".join(parts[2:])
        bn = rest.endswith("_BN"); rest = rest[:-3] if bn else rest
        base = f"MobilenetV2/expanded_conv_{i}/{rest}"
        if bn: return base + "/BatchNorm", "bn"
        return base, ("dwconv" if rest == "depthwise" else "conv")
    return None, None


def _download_savedmodel():
    if os.path.isfile(SAVEDMODEL_PB):
        return
    import tarfile
    import io
    os.makedirs(os.path.dirname(SAVEDMODEL_PB), exist_ok=True)
    print(f"[inat] downloading AIY birds_V1 SavedModel from tfhub ...")
    with urllib.request.urlopen(HUB_URL) as r:
        buf = r.read()
    with tarfile.open(fileobj=io.BytesIO(buf), mode="r:gz") as tar:
        tar.extractall(os.path.dirname(SAVEDMODEL_PB))
    if not os.path.isfile(SAVEDMODEL_PB):
        sys.exit("[inat] extraction did not yield saved_model.pb")
    print(f"[inat] cached {SAVEDMODEL_PB}")


def build_cache():
    """Parse the frozen graph's Const weights and save them to CACHE_NPZ."""
    import numpy as np
    import tensorflow as tf
    from tensorflow.core.protobuf import saved_model_pb2
    _download_savedmodel()
    sm = saved_model_pb2.SavedModel()
    with open(SAVEDMODEL_PB, "rb") as f:
        sm.ParseFromString(f.read())
    gd = sm.meta_graphs[0].graph_def
    out = {}
    for n in gd.node:
        if n.op == "Const" and "value" in n.attr and n.name.startswith("MobilenetV2"):
            arr = tf.make_ndarray(n.attr["value"].tensor)
            if arr.size >= 16:
                out[n.name] = arr
    if len(out) != 260:
        print(f"[inat] WARNING: expected 260 weight tensors, got {len(out)}")
    np.savez(CACHE_NPZ, **out)
    print(f"[inat] wrote {CACHE_NPZ}  ({len(out)} tensors)")
    return out


def load_into(keras_backbone) -> int:
    """Assign cached iNat weights into a Keras MobileNetV2(include_top=False).

    Returns the number of layers assigned. Raises if a mapped tensor is absent
    (i.e. the cache is for a different alpha/architecture)."""
    import numpy as np
    if not os.path.isfile(CACHE_NPZ):
        build_cache()
    C = dict(np.load(CACHE_NPZ))
    assigned = 0
    for L in keras_backbone.layers:
        if not L.get_weights():
            continue
        pfx, kind = _slim_prefix(L.name)
        if pfx is None:
            raise ValueError(f"[inat] unmapped backbone layer: {L.name}")
        if kind == "conv":
            L.set_weights([C[pfx + "/weights"]])
        elif kind == "dwconv":
            L.set_weights([C[pfx + "/depthwise_weights"]])
        elif kind == "bn":
            L.set_weights([C[pfx + "/gamma"], C[pfx + "/beta"],
                           C[pfx + "/moving_mean"], C[pfx + "/moving_variance"]])
        assigned += 1
    print(f"[inat] transplanted iNat weights into {assigned} backbone layers")
    return assigned


if __name__ == "__main__":
    build_cache()
