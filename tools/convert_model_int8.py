# Convert a fully uint8-quantized TFLite classifier (e.g. Google Coral's
# mobilenet_v2_1.0_224_inat_bird_quant.tflite) to full int8 for modern
# TFLite-Micro, whose kernels no longer accept uint8 models.
#
# Usage: python convert_model_int8.py <in.tflite> <out.tflite>
# Needs: numpy, flatbuffers (pip install numpy flatbuffers); uses the
# bundled schema_py_generated.py (Apache-2.0, from tensorflow/tflite-micro).
#
# - Activation tensors: type uint8 -> int8, zero_point -= 128, scale kept
#   (a pure representation shift, numerically lossless).
# - Filter tensors (CONV_2D dim 0, DEPTHWISE_CONV_2D dim 3): dequantize with
#   the original asymmetric per-tensor params, requantize per-channel
#   symmetric int8 (zp == 0, required by TFLM int8 kernels).
# - FULLY_CONNECTED filter: per-tensor symmetric.
# - Bias tensors: recomputed from the exact float bias so that
#   bias_scale[c] = input_scale * filter_scale[c].
import sys
import numpy as np
import flatbuffers
import schema_py_generated as tfl

if len(sys.argv) != 3:
    sys.exit("usage: convert_model_int8.py <in.tflite> <out.tflite>")
SRC, DST = sys.argv[1], sys.argv[2]

UINT8, INT8, INT32 = tfl.TensorType.UINT8, tfl.TensorType.INT8, tfl.TensorType.INT32

data = open(SRC, "rb").read()
model = tfl.ModelT.InitFromPackedBuf(bytearray(data), 0)
sg = model.subgraphs[0]
tensors, buffers, opcodes = sg.tensors, model.buffers, model.operatorCodes


def opname(op):
    oc = opcodes[op.opcodeIndex]
    code = max(oc.builtinCode, oc.deprecatedBuiltinCode)
    for name in dir(tfl.BuiltinOperator):
        if not name.startswith("_") and getattr(tfl.BuiltinOperator, name) == code:
            return name
    return str(code)


def buf_data(ti):
    b = buffers[tensors[ti].buffer]
    return None if b.data is None or len(b.data) == 0 else np.frombuffer(bytes(b.data), dtype=np.uint8)


# ---- classify every tensor's role -------------------------------------
filter_of = {}   # tensor idx -> (op name, quantized dim or None for per-tensor)
bias_of = {}     # bias tensor idx -> (input tensor idx, filter tensor idx)
for op in sg.operators:
    name = opname(op)
    if name in ("CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"):
        inp, filt = op.inputs[0], op.inputs[1]
        qdim = {"CONV_2D": 0, "DEPTHWISE_CONV_2D": 3, "FULLY_CONNECTED": None}[name]
        filter_of[filt] = (name, qdim)
        if len(op.inputs) > 2 and op.inputs[2] >= 0:
            bias_of[op.inputs[2]] = (inp, filt)

# sanity: no other op should consume a constant uint8 tensor
for op in sg.operators:
    name = opname(op)
    if name in ("CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"):
        continue
    for ti in op.inputs:
        if ti >= 0 and tensors[ti].type == UINT8 and buf_data(ti) is not None:
            sys.exit(f"FATAL: op {name} consumes constant uint8 tensor {ti}")

# ---- pass 1: requantize filters ---------------------------------------
new_filter_scale = {}   # filter idx -> per-channel float64 array
worst_err = 0.0
for fi, (name, qdim) in filter_of.items():
    t = tensors[fi]
    q = t.quantization
    old_scale, old_zp = q.scale[0], q.zeroPoint[0]
    w_u8 = buf_data(fi).reshape(t.shape)
    w_f = (w_u8.astype(np.float64) - old_zp) * old_scale

    if qdim is None:  # FULLY_CONNECTED: per-tensor symmetric
        s = max(np.abs(w_f).max() / 127.0, 1e-12)
        w_i8 = np.clip(np.round(w_f / s), -127, 127).astype(np.int8)
        scales = np.array([s])
        q.quantizedDimension = 0
    else:
        nchan = t.shape[qdim]
        w_moved = np.moveaxis(w_f, qdim, 0).reshape(nchan, -1)
        s_c = np.maximum(np.abs(w_moved).max(axis=1) / 127.0, 1e-12)
        w_q = np.clip(np.round(w_moved / s_c[:, None]), -127, 127)
        w_i8 = np.moveaxis(w_q.reshape([nchan] + list(np.moveaxis(w_f, qdim, 0).shape[1:])), 0, qdim).astype(np.int8)
        scales = s_c
        q.quantizedDimension = qdim

    # numeric self-check: reconstruction error must be <= scale/2 per channel
    if qdim is None:
        err = np.abs(w_i8.astype(np.float64) * scales[0] - w_f).max() / scales[0]
    else:
        rec = np.moveaxis(w_i8.astype(np.float64), qdim, 0).reshape(len(scales), -1) * scales[:, None]
        err = (np.abs(rec - np.moveaxis(w_f, qdim, 0).reshape(len(scales), -1)).max(axis=1) / scales).max()
    worst_err = max(worst_err, err)

    buffers[t.buffer].data = w_i8.tobytes()
    t.type = INT8
    q.scale = scales.astype(np.float32).tolist()
    q.zeroPoint = [0] * len(scales)
    q.min, q.max = None, None
    new_filter_scale[fi] = scales

# ---- pass 2: recompute biases ------------------------------------------
for bi, (ii, fi) in bias_of.items():
    t = tensors[bi]
    assert t.type == INT32, f"bias tensor {bi} is not int32"
    q = t.quantization
    old_bscale = q.scale[0]
    b_i32 = np.frombuffer(bytes(buffers[t.buffer].data), dtype=np.int32)
    b_f = b_i32.astype(np.float64) * old_bscale  # exact float bias

    in_scale = tensors[ii].quantization.scale[0]
    w_scales = new_filter_scale[fi]
    b_scales = in_scale * (w_scales if len(w_scales) > 1 else np.full(len(b_i32), w_scales[0]))
    b_new = np.clip(np.round(b_f / b_scales), -2**31, 2**31 - 1).astype(np.int32)

    buffers[t.buffer].data = b_new.tobytes()
    q.scale = b_scales.astype(np.float32).tolist()
    q.zeroPoint = [0] * len(b_scales)
    q.quantizedDimension = 0
    q.min, q.max = None, None

# ---- pass 3: shift remaining uint8 tensors (activations) ---------------
n_act = 0
for t in tensors:
    if t.type != UINT8:
        continue
    t.type = INT8
    q = t.quantization
    if q is not None and q.zeroPoint is not None:
        q.zeroPoint = [zp - 128 for zp in q.zeroPoint]
        q.min, q.max = None, None
    n_act += 1

# ---- verify softmax output params (TFLM requires 1/256, zp -128) -------
out_t = tensors[sg.outputs[0]]
oq = out_t.quantization
print(f"output tensor: scale={oq.scale[0]}, zp={oq.zeroPoint[0]} (need 1/256={1/256}, -128)")
in_t = tensors[sg.inputs[0]]
print(f"input tensor:  scale={in_t.quantization.scale[0]}, zp={in_t.quantization.zeroPoint[0]}")

# ---- repack -------------------------------------------------------------
b = flatbuffers.Builder(4 * 1024 * 1024)
off = model.Pack(b)
b.Finish(off, file_identifier=b"TFL3")
out = bytes(b.Output())
open(DST, "wb").write(out)
print(f"filters requantized: {len(filter_of)}, biases: {len(bias_of)}, activations shifted: {n_act}")
print(f"worst weight reconstruction error: {worst_err:.4f} LSB (must be <= 0.5)")
print(f"wrote {DST}: {len(out)} bytes (src {len(data)})")
