"""Export YOLOv8n weights as INT8 (W8A32) for Direct2Metal bare-metal.

Mirrors tools/export_model.py's explicit graph walk so the layer order is
identical between FP32 and INT8 bins — the kernel can switch between them
without re-deriving consumption order.

Per-Conv2d emission format (each layer):
    uint32  count_w     (total int8 elements in repacked weight tensor)
    float32 scale       (per-tensor symmetric: w_fp = w_int * scale)
    int8    weights[count_w]
    uint32  count_b     (= C_out, always emitted, zeros if no bias)
    float32 bias[count_b]   (kept FP32 — added to FP32 accumulator)

Quantization is per-tensor symmetric (no zero-point). For W8A32 this is
the cheapest scheme: dequant in the conv inner loop is a single
vmulq_n_f32 per 4 weights.
"""

import os
import struct
import zlib

import numpy as np
import torch
import torch.nn as nn

from ultralytics import YOLO
from ultralytics.nn.modules import C2f, Conv, SPPF, Bottleneck, Detect


# ---------------------------------------------------------------------------
# Packing helpers — must match the FP32 path exactly for shape compatibility.
# ---------------------------------------------------------------------------
def repack_int8_4ch(w_int8):
    C_out, C_in, K, _ = w_int8.shape
    C_out_pad = ((C_out + 3) // 4) * 4
    w = w_int8.reshape(C_out, C_in * K * K).astype(np.int8)
    if C_out_pad > C_out:
        w = np.concatenate(
            [w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype=np.int8)], axis=0
        )
    w = w.reshape(C_out_pad // 4, 4, C_in * K * K)
    w = w.transpose(0, 2, 1)
    return w.flatten()


def repack_int8_8ch(w_int8):
    C_out, C_in, K, _ = w_int8.shape
    C_out_pad = ((C_out + 7) // 8) * 8
    w = w_int8.reshape(C_out, C_in * K * K).astype(np.int8)
    if C_out_pad > C_out:
        w = np.concatenate(
            [w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype=np.int8)], axis=0
        )
    w = w.reshape(C_out_pad // 8, 8, C_in * K * K)
    w = w.transpose(0, 2, 1)
    return w.flatten()


# Per-layer audit log, printed once at the end so we can spot bad scales.
_LAYER_STATS = []


def emit_conv(f, conv: nn.Conv2d, tag: str):
    w = conv.weight.detach().cpu().numpy().astype(np.float32)
    C_out, C_in, K_h, K_w = w.shape
    assert K_h == K_w, f"non-square kernel at {tag}: {K_h}x{K_w}"
    K = K_h

    # Per-tensor symmetric quantization: scale = max|w| / 127.
    max_abs = float(np.max(np.abs(w))) if w.size else 0.0
    scale = max_abs / 127.0 if max_abs > 0 else 1.0
    w_q = np.clip(np.round(w / scale), -127, 127).astype(np.int8)

    # Saturation fraction — sanity check on the chosen scale.
    sat = float(np.mean(np.abs(w_q) == 127))

    if K > 1 and C_out % 8 == 0 and C_out >= 32:
        packed = repack_int8_8ch(w_q)
        layout = "8ch"
    elif K > 1 and C_out % 4 == 0:
        packed = repack_int8_4ch(w_q)
        layout = "4ch"
    else:
        packed = w_q.flatten()
        layout = "flat"

    f.write(struct.pack("<I", len(packed)))
    f.write(struct.pack("<f", scale))
    f.write(packed.tobytes())

    if conv.bias is not None:
        b = conv.bias.detach().cpu().numpy().astype(np.float32)
    else:
        b = np.zeros(C_out, dtype=np.float32)
    f.write(struct.pack("<I", len(b)))
    f.write(b.tobytes())

    _LAYER_STATS.append(
        (tag, K, C_in, C_out, layout, scale, max_abs, sat, len(packed))
    )


# ---------------------------------------------------------------------------
# Block-level emitters — identical traversal order to export_model.py.
# ---------------------------------------------------------------------------
def emit_conv_block(f, conv_module: Conv, tag: str):
    emit_conv(f, conv_module.conv, tag)


def emit_bottleneck(f, bn: Bottleneck, tag: str):
    emit_conv_block(f, bn.cv1, f"{tag}.cv1")
    emit_conv_block(f, bn.cv2, f"{tag}.cv2")


def emit_c2f(f, c2f_mod: C2f, tag: str):
    emit_conv_block(f, c2f_mod.cv1, f"{tag}.cv1")
    emit_conv_block(f, c2f_mod.cv2, f"{tag}.cv2")
    for i, bn in enumerate(c2f_mod.m):
        emit_bottleneck(f, bn, f"{tag}.m{i}")


def emit_sppf(f, sppf_mod: SPPF, tag: str):
    emit_conv_block(f, sppf_mod.cv1, f"{tag}.cv1")
    emit_conv_block(f, sppf_mod.cv2, f"{tag}.cv2")


def emit_detect(f, det: Detect, tag: str):
    for i, seq in enumerate(det.cv2):
        emit_conv_block(f, seq[0], f"{tag}.cv2[{i}].0")
        emit_conv_block(f, seq[1], f"{tag}.cv2[{i}].1")
        emit_conv(f,       seq[2], f"{tag}.cv2[{i}].2")
    for i, seq in enumerate(det.cv3):
        emit_conv_block(f, seq[0], f"{tag}.cv3[{i}].0")
        emit_conv_block(f, seq[1], f"{tag}.cv3[{i}].1")
        emit_conv(f,       seq[2], f"{tag}.cv3[{i}].2")
    # dfl.conv is fixed [0..15], baked into the decoder.


def crc32_of_file(path):
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def write_weights_crc_header(weights_path, header_path):
    crc = crc32_of_file(weights_path)
    size = os.path.getsize(weights_path)
    with open(header_path, "w") as f:
        f.write("/* Auto-generated by tools/export_int8.py — do not edit manually */\n")
        f.write("#ifndef WEIGHTS_INT8_CRC_H\n")
        f.write("#define WEIGHTS_INT8_CRC_H\n\n")
        f.write(f"#define WEIGHTS_INT8_CRC32 0x{crc:08X}U\n")
        f.write(f"#define WEIGHTS_INT8_SIZE  {size}U\n\n")
        f.write("#endif /* WEIGHTS_INT8_CRC_H */\n")
    print(f"  weights_int8_crc.h: CRC32=0x{crc:08X}, SIZE={size} bytes")


def main():
    print("=== Exporting YOLOv8n Weights (INT8 W8A32, per-tensor sym) ===")
    yolo = YOLO("yolov8n.pt")
    model = yolo.model
    model.eval()
    model.fuse()
    seq = model.model

    # V201: `src/` se retiró en V171 (GOALS.md G1 paso 5) — los blobs viven en
    # `app/<APP>/`. Este exportador se quedó apuntando al directorio viejo desde
    # entonces; `convert_image.py` sí se arregló (V184). Misma convención que
    # aquél: raíz del repo + app/<APP>, con `--app` para sobreescribir.
    app_name = os.environ.get("D2M_APP", "yolo_v8n_coco")
    root     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src_dir  = os.path.join(root, "app", app_name)
    if not os.path.isdir(src_dir):
        raise SystemExit(f"no existe {src_dir} (¿APP mal puesto? usa D2M_APP=<nombre>)")
    bin_path = os.path.join(src_dir, "weights_int8.bin")

    with open(bin_path, "wb") as f:
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

    print(f"\nDone. Exported to {bin_path}")
    print(f"  Layers emitted: {len(_LAYER_STATS)}")

    # Audit log: per-layer scale + saturation %.
    print("\nPer-layer quantization audit:")
    print(f"  {'tag':<36} K  Cin  Cout  layout  scale            max|w|     sat%")
    for tag, K, Cin, Cout, layout, scale, max_abs, sat, _ in _LAYER_STATS:
        print(
            f"  {tag:<36} {K} {Cin:<4} {Cout:<5} {layout:<6} "
            f"{scale:.6e}    {max_abs:.4f}   {sat * 100:.2f}"
        )

    write_weights_crc_header(bin_path, os.path.join(src_dir, "weights_int8_crc.h"))


if __name__ == "__main__":
    main()
