"""Tier 2 (W8A8) calibration + export for Direct2Metal.

Runs ~50 representative images through YOLOv8n in FP32 with forward
hooks on each Conv module to capture per-tensor symmetric activation
scales. Then writes `weights_int8_w8a8.bin` containing per-layer
(scale_w, scale_in, scale_out, int8 weights, int32 bias). The int8
kernel can multiply-accumulate purely in int32 and emit the result
directly to int8 in the next layer's scale.

Bin format per Conv2d (in the same traversal order as export_int8.py):

    uint32  n_w               (NEON-repacked int8 element count)
    float   scale_w           (w_fp     = w_int8 * scale_w)
    float   scale_in          (in_fp    = in_int8 * scale_in)
    float   scale_out         (out_fp   = out_int8 * scale_out)
    int8    weights[n_w]
    uint32  n_b
    int32   bias[n_b]         (= round(bias_fp / (scale_in * scale_w)))

Bias is pre-multiplied so it can be added directly to the int32 conv
accumulator without floating-point ops in the hot loop.

Usage:
    tools/env/bin/python tools/calibrate_int8.py
    tools/env/bin/python tools/calibrate_int8.py --source path/to/imgs/ --n_images 100
"""

import argparse
import os
import struct
import sys
import zlib
from collections import defaultdict
from pathlib import Path

import numpy as np
import torch
from torch import nn
from PIL import Image

from ultralytics import YOLO
from ultralytics.nn.modules import Conv, C2f, SPPF, Bottleneck, Detect

# Reuse repack helpers from export_int8.py so the layout stays bit-exact.
sys.path.insert(0, str(Path(__file__).parent))
from export_int8 import repack_int8_4ch, repack_int8_8ch  # noqa: E402


# Activation stats keyed by Conv2d id(). Outer-module hooks fill both
# fields per layer. After calibration we read scale_in / scale_out from
# here in the export pass.
_STATS = defaultdict(lambda: {"in_absmax": 0.0, "out_absmax": 0.0})

# Global multiplicative bump applied to every activation scale. Used to
# add headroom when the calibration set (coco128) underestimates
# runtime activation magnitudes vs the actual deployment camera. Bias
# int32 is recomputed against the bumped scales so the chain stays
# consistent.
_SCALE_SAFETY = 1.0


def setup_hooks(model):
    """Hook every Conv2d's outer module (Ultralytics Conv wrapper if present;
    bare Conv2d otherwise). Pre-hook records input absmax; post-hook records
    output absmax. The wrapper's output is post-SiLU so scale_out reflects
    what the next layer actually reads."""
    modules_dict = dict(model.named_modules())
    handles = []

    for name, m in modules_dict.items():
        if not isinstance(m, nn.Conv2d):
            continue
        conv_id = id(m)
        parent_name = ".".join(name.split(".")[:-1])
        parent = modules_dict.get(parent_name)
        outer = parent if isinstance(parent, Conv) else m

        def make_pre_hook(_id):
            def hook(_module, inputs):
                v = float(inputs[0].detach().abs().max())
                if v > _STATS[_id]["in_absmax"]:
                    _STATS[_id]["in_absmax"] = v
            return hook

        def make_post_hook(_id):
            def hook(_module, _inputs, output):
                v = float(output.detach().abs().max())
                if v > _STATS[_id]["out_absmax"]:
                    _STATS[_id]["out_absmax"] = v
            return hook

        handles.append(outer.register_forward_pre_hook(make_pre_hook(conv_id)))
        handles.append(outer.register_forward_hook(make_post_hook(conv_id)))

    return handles


def get_image_paths(source, n_max):
    if source == "coco128":
        from ultralytics.data.utils import check_det_dataset
        data = check_det_dataset("coco128.yaml")
        train_path = Path(data["train"])
        images = sorted(train_path.glob("*.jpg"))
    else:
        src = Path(source)
        if not src.exists():
            raise FileNotFoundError(src)
        images = sorted(list(src.glob("*.jpg")) + list(src.glob("*.png")))
    if not images:
        raise RuntimeError(f"No images found in '{source}'")
    return images[:n_max]


def run_calibration(model, image_paths, input_size):
    for i, path in enumerate(image_paths):
        if i % 10 == 0:
            print(f"  [{i:3d}/{len(image_paths)}] {path.name}")
        img = Image.open(path).convert("RGB").resize((input_size, input_size))
        arr = np.asarray(img).astype(np.float32) / 255.0
        t = torch.from_numpy(arr.transpose(2, 0, 1)).unsqueeze(0)
        with torch.no_grad():
            _ = model(t)


# ── Per-Conv2d emit (mirrors export_int8.py traversal) ─────────────────────
_LAYER_STATS = []


def emit_conv_w8a8(f, conv, tag):
    w = conv.weight.detach().cpu().numpy().astype(np.float32)
    C_out, C_in, K_h, K_w = w.shape
    assert K_h == K_w, f"non-square kernel at {tag}: {K_h}x{K_w}"
    K = K_h

    max_abs_w = float(np.max(np.abs(w))) if w.size else 0.0
    scale_w = max_abs_w / 127.0 if max_abs_w > 0 else 1.0
    w_q = np.clip(np.round(w / scale_w), -127, 127).astype(np.int8)
    sat_w = float(np.mean(np.abs(w_q) == 127))

    st = _STATS.get(id(conv))
    if st is None or st["in_absmax"] == 0.0:
        print(f"  WARN: no activation stats for {tag}; default scale_in = 1/127")
        scale_in = 1.0 / 127.0
    else:
        scale_in = st["in_absmax"] / 127.0
    if st is None or st["out_absmax"] == 0.0:
        scale_out = 1.0 / 127.0
    else:
        scale_out = st["out_absmax"] / 127.0

    # Apply the global safety bump (default 1.0 = no change). Scaling
    # both ends keeps the layer-to-layer chain consistent: layer N+1's
    # scale_in matches layer N's scale_out up to whatever ratio the
    # calibration produced.
    scale_in  *= _SCALE_SAFETY
    scale_out *= _SCALE_SAFETY

    if K > 1 and C_out % 8 == 0 and C_out >= 32:
        packed = repack_int8_8ch(w_q); layout = "8ch"
    elif K > 1 and C_out % 4 == 0:
        packed = repack_int8_4ch(w_q); layout = "4ch"
    else:
        packed = w_q.flatten(); layout = "flat"

    if conv.bias is not None:
        b_fp = conv.bias.detach().cpu().numpy().astype(np.float32)
    else:
        b_fp = np.zeros(C_out, dtype=np.float32)

    bias_divisor = scale_in * scale_w
    b_q32 = np.clip(np.round(b_fp / bias_divisor),
                    -(2**31), 2**31 - 1).astype(np.int32)
    bias_max_abs_fp = float(np.max(np.abs(b_fp))) if b_fp.size else 0.0
    bias_max_abs_q  = int(np.max(np.abs(b_q32))) if b_q32.size else 0

    f.write(struct.pack("<I", len(packed)))
    f.write(struct.pack("<f", scale_w))
    f.write(struct.pack("<f", scale_in))
    f.write(struct.pack("<f", scale_out))
    f.write(packed.tobytes())
    f.write(struct.pack("<I", len(b_q32)))
    f.write(b_q32.tobytes())

    _LAYER_STATS.append({
        "tag": tag, "K": K, "C_in": C_in, "C_out": C_out, "layout": layout,
        "scale_w": scale_w, "scale_in": scale_in, "scale_out": scale_out,
        "sat_w": sat_w, "n_packed": len(packed),
        "bias_max_fp": bias_max_abs_fp, "bias_max_q": bias_max_abs_q,
    })


def emit_conv_block(f, conv_module, tag):
    emit_conv_w8a8(f, conv_module.conv, tag)


def emit_bottleneck(f, bn, tag):
    emit_conv_block(f, bn.cv1, f"{tag}.cv1")
    emit_conv_block(f, bn.cv2, f"{tag}.cv2")


def emit_c2f(f, c2f_mod, tag):
    emit_conv_block(f, c2f_mod.cv1, f"{tag}.cv1")
    emit_conv_block(f, c2f_mod.cv2, f"{tag}.cv2")
    for i, bn in enumerate(c2f_mod.m):
        emit_bottleneck(f, bn, f"{tag}.m{i}")


def emit_sppf(f, sppf_mod, tag):
    emit_conv_block(f, sppf_mod.cv1, f"{tag}.cv1")
    emit_conv_block(f, sppf_mod.cv2, f"{tag}.cv2")


def emit_detect(f, det, tag):
    for i, seq in enumerate(det.cv2):
        emit_conv_block(f, seq[0], f"{tag}.cv2[{i}].0")
        emit_conv_block(f, seq[1], f"{tag}.cv2[{i}].1")
        emit_conv_w8a8 (f, seq[2], f"{tag}.cv2[{i}].2")
    for i, seq in enumerate(det.cv3):
        emit_conv_block(f, seq[0], f"{tag}.cv3[{i}].0")
        emit_conv_block(f, seq[1], f"{tag}.cv3[{i}].1")
        emit_conv_w8a8 (f, seq[2], f"{tag}.cv3[{i}].2")


def crc32_of_file(path):
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def write_crc_header(bin_path, header_path, prefix="WEIGHTS_INT8_W8A8"):
    crc = crc32_of_file(bin_path)
    size = os.path.getsize(bin_path)
    with open(header_path, "w") as f:
        f.write("/* Auto-generated by tools/calibrate_int8.py — do not edit manually */\n")
        f.write(f"#ifndef {prefix}_CRC_H\n")
        f.write(f"#define {prefix}_CRC_H\n\n")
        f.write(f"#define {prefix}_CRC32 0x{crc:08X}U\n")
        f.write(f"#define {prefix}_SIZE  {size}U\n\n")
        f.write(f"#endif /* {prefix}_CRC_H */\n")
    print(f"  {header_path.name}: CRC32=0x{crc:08X}, SIZE={size} bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n_images", type=int, default=50)
    ap.add_argument("--source", type=str, default="coco128",
                    help="'coco128' (auto-download) or a folder of jpg/png images.")
    ap.add_argument("--input_size", type=int, default=256,
                    help="YOLO input resolution; must match YOLO_IN in bsp.h.")
    ap.add_argument("--scale_safety_margin", type=float, default=1.0,
                    help="Multiply every activation scale by this factor. "
                         "Use >1.0 (e.g. 1.5) when HW saturates mid-backbone "
                         "because deployment camera magnitudes exceed the "
                         "calibration set's absmax. Trades a bit of int8 "
                         "precision on under-using layers for headroom on "
                         "saturating ones.")
    ap.add_argument("--output", type=Path,
                    default=Path("app/yolo_v8n_coco/weights_int8_w8a8.bin"))
    ap.add_argument("--crc_header", type=Path,
                    default=Path("app/yolo_v8n_coco/weights_int8_w8a8_crc.h"))
    args = ap.parse_args()

    global _SCALE_SAFETY
    _SCALE_SAFETY = args.scale_safety_margin

    print("=== Tier 2 W8A8 calibration + export ===")
    print(f"  source        : {args.source}")
    print(f"  n_images      : {args.n_images}")
    print(f"  input_size    : {args.input_size}")
    print(f"  safety_margin : {args.scale_safety_margin}")
    print(f"  output        : {args.output}")
    print(f"  crc_header    : {args.crc_header}")

    print("\nLoading YOLOv8n...")
    yolo = YOLO("yolov8n.pt")
    model = yolo.model
    model.eval()
    model.fuse()
    seq = model.model

    print("\nRegistering forward hooks on every Conv module...")
    handles = setup_hooks(model)
    print(f"  hooks installed: {len(handles)} ({len(handles)//2} pre + post)")

    print(f"\nCalibrating on '{args.source}' (target {args.n_images} images)...")
    image_paths = get_image_paths(args.source, args.n_images)
    print(f"  resolved {len(image_paths)} image paths")
    run_calibration(model, image_paths, args.input_size)

    for h in handles:
        h.remove()

    print(f"\nExporting W8A8 weights → {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "wb") as f:
        print("Backbone:")
        emit_conv_block(f, seq[0],  "L0_Conv3x3_3_16")
        emit_conv_block(f, seq[1],  "L1_Conv3x3_16_32")
        emit_c2f       (f, seq[2],  "L2_C2f_32_32_n1")
        emit_conv_block(f, seq[3],  "L3_Conv3x3_32_64")
        emit_c2f       (f, seq[4],  "L4_C2f_64_64_n2")
        emit_conv_block(f, seq[5],  "L5_Conv3x3_64_128")
        emit_c2f       (f, seq[6],  "L6_C2f_128_128_n2")
        emit_conv_block(f, seq[7],  "L7_Conv3x3_128_256")
        emit_c2f       (f, seq[8],  "L8_C2f_256_256_n1")
        emit_sppf      (f, seq[9],  "L9_SPPF_256")

        print("Neck:")
        emit_c2f       (f, seq[12], "L12_C2f_384_128_n1")
        emit_c2f       (f, seq[15], "L15_C2f_192_64_n1_P3")
        emit_conv_block(f, seq[16], "L16_Conv3x3_64_64_s2")
        emit_c2f       (f, seq[18], "L18_C2f_192_128_n1_P4")
        emit_conv_block(f, seq[19], "L19_Conv3x3_128_128_s2")
        emit_c2f       (f, seq[21], "L21_C2f_384_256_n1_P5")

        print("Head:")
        emit_detect    (f, seq[22], "L22_Detect")

    print(f"\nDone. Layers emitted: {len(_LAYER_STATS)}")

    print("\nPer-layer scales (audit):")
    print(f"  {'tag':<36} K  Cin  Cout  layout  "
          f"scale_w    scale_in   scale_out  sat_w%  |bias|fp  |bias|q")
    for L in _LAYER_STATS:
        print(f"  {L['tag']:<36} {L['K']} {L['C_in']:<4} {L['C_out']:<5} "
              f"{L['layout']:<6} "
              f"{L['scale_w']:.2e}  {L['scale_in']:.2e}  {L['scale_out']:.2e}  "
              f"{L['sat_w']*100:5.1f}  "
              f"{L['bias_max_fp']:.2e}  {L['bias_max_q']}")

    write_crc_header(args.output, args.crc_header)


if __name__ == "__main__":
    main()
