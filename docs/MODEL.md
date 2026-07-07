# Species-ID model setup (FSD §3.2)

BirdBox classifies each visit's best frame on-device with a quantized
TFLite image classifier loaded from the microSD card. No model is baked
into the firmware — you install one once, and can swap it (e.g. for a
different region) without reflashing.

## Recommended model (v1 default)

Google's **iNaturalist birds** classifier — MobileNetV2, quantized,
965 bird species worldwide (including the common European garden/feeder
species) plus a `background` guard class. Apache-2.0 licensed.

Download both files:

- Model: <https://raw.githubusercontent.com/google-coral/test_data/master/mobilenet_v2_1.0_224_inat_bird_quant.tflite>
- Labels: <https://raw.githubusercontent.com/google-coral/test_data/master/inat_bird_labels.txt>

### Convert to int8 (required)

The Coral download is uint8-quantized, which modern TFLite-Micro no
longer runs ("Hybrid models are not supported"). Convert it once with
the bundled script — it requantizes the weights per-channel symmetric
int8 exactly (no calibration data needed, worst-case error half an LSB):

```
pip install numpy flatbuffers
python tools/convert_model_int8.py \
    mobilenet_v2_1.0_224_inat_bird_quant.tflite inat-birds-v1.tflite
```

The converted model is ~3.9 MB (per-channel scales add metadata).

## Installing

**Option A — SD card in a PC:** copy the converted model and the labels
file into the card's `model/` folder as a matching pair, e.g.
`inat-birds-v1.tflite` and `inat-birds-v1.txt` (labels file = model
filename with `.txt`).

**Option B — over the network** (device stays mounted):

```
curl -X POST --data-binary @inat-birds-v1.tflite \
     "http://<device-ip>/model/upload?name=inat-birds-v1.tflite"
curl -X POST --data-binary @inat_bird_labels.txt \
     "http://<device-ip>/model/upload?name=inat-birds-v1.txt"
curl -X POST http://<device-ip>/api/reboot
```

After the reboot, the Debug tab's Species ID card shows the loaded model
and label count. With several models on the card, pick the active one in
**Settings → Region / species model**.

## Testing it

POST any **baseline** JPEG (≤ 300 KB; progressive JPEGs won't decode —
camera frames are always baseline) and get the top-3 back:

```
curl -X POST --data-binary @some-bird.jpg http://<device-ip>/api/classify
```

Reference results on the stock model (960px Wikimedia photos, ~4.3 s
per inference): a clear European Robin scores 87%, a clear Eurasian
Blue Tit 86%; distant or obstructed birds fall below the confidence
threshold and are logged as `Unidentified bird`.

## Model requirements

Any TFLite image classifier works if it is:

- fully int8-quantized (input **and** output) — uint8 models must go
  through `tools/convert_model_int8.py` first,
- input `1x224x224x3` RGB,
- built from ops in: `CONV_2D`, `DEPTHWISE_CONV_2D`, `ADD`,
  `AVERAGE_POOL_2D`, `FULLY_CONNECTED`, `SOFTMAX`
  (MobileNet-family models qualify),
- ≤ ~4.5 MB, with an index-aligned labels `.txt` (one label per line;
  a literal `background` label is treated as the no-bird guard class).

Labels of the form `Scientific name (Common Name)` display as the common
name; anything else is shown as-is.

## Display language

Settings → **Species name language** switches the common name between
English and Norwegian; the scientific (Latin) binomial is always shown
alongside it either way, so a result is never ambiguous. Norwegian names
come from a curated table in `main/species_i18n.c` covering common
Northern-European garden/feeder/nestbox species — species outside that
table still display correctly (English name + Latin), they just don't
have a Norwegian translation yet. Changing the language re-labels past
visits immediately; nothing needs re-classifying.
