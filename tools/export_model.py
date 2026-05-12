"""Export YOLOv8n weights for Direct2Metal bare-metal inference.

The kernel consumes layers via WeightStream in the exact order they are written
here. Iteration order is NOT taken from `named_modules()` — we walk the v8
graph explicitly so the layout is stable and reviewable.

Per-Conv2d emission format:
    uint32  count_w     (total float32 elements in repacked tensor)
    float32 weights[]   (packed: 8ch-NEON if K>1 & C_out>=32 & C_out%8==0,
                                 4ch-NEON if K>1 & C_out%4==0,
                                 flat (PyTorch order) otherwise)
    uint32  count_b     (= C_out, always emitted, zeros if no bias)
    float32 bias[]      (length C_out)
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
# Low-level packing helpers (unchanged from v5 exporter)
# ---------------------------------------------------------------------------
def repack_for_neon(weight_np):
    C_out, C_in, K, _ = weight_np.shape
    C_out_pad = ((C_out + 3) // 4) * 4
    w = weight_np.reshape(C_out, C_in * K * K).astype("float32")
    if C_out_pad > C_out:
        w = np.concatenate(
            [w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype="float32")], axis=0
        )
    w = w.reshape(C_out_pad // 4, 4, C_in * K * K)
    w = w.transpose(0, 2, 1)
    return w.flatten()


def repack_for_neon_8ch(weight_np):
    C_out, C_in, K, _ = weight_np.shape
    C_out_pad = ((C_out + 7) // 8) * 8
    w = weight_np.reshape(C_out, C_in * K * K).astype("float32")
    if C_out_pad > C_out:
        w = np.concatenate(
            [w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype="float32")], axis=0
        )
    w = w.reshape(C_out_pad // 8, 8, C_in * K * K)
    w = w.transpose(0, 2, 1)
    return w.flatten()


def write_tensor(f, arr):
    flat = arr.astype(np.float32).flatten()
    f.write(struct.pack("<I", len(flat)))
    f.write(flat.tobytes())


def emit_conv(f, conv: nn.Conv2d, tag: str):
    """Emit a single Conv2d (post-BN-fuse) following kernel layout rules."""
    w = conv.weight.detach().cpu().numpy()
    C_out, C_in, K_h, K_w = w.shape
    assert K_h == K_w, f"non-square kernel at {tag}: {K_h}x{K_w}"
    K = K_h

    if K > 1 and C_out % 8 == 0 and C_out >= 32:
        packed = repack_for_neon_8ch(w)
    elif K > 1 and C_out % 4 == 0:
        packed = repack_for_neon(w)
    else:
        packed = w.astype("float32").flatten()
    write_tensor(f, packed)

    if conv.bias is not None:
        b = conv.bias.detach().cpu().numpy()
    else:
        b = np.zeros(C_out, dtype=np.float32)
    write_tensor(f, b)

    print(f"  emit {tag:<24} K={K} C_in={C_in:<3} C_out={C_out:<3} elems={len(packed)}")


# ---------------------------------------------------------------------------
# Block-level emitters (match exactly the kernel consumption order)
# ---------------------------------------------------------------------------
def emit_conv_block(f, conv_module: Conv, tag: str):
    """Ultralytics `Conv` = Conv2d + BN + SiLU. After fuse(), BN is folded
    into Conv2d (with bias). We just emit the underlying conv."""
    emit_conv(f, conv_module.conv, tag)


def emit_bottleneck(f, bn: Bottleneck, tag: str):
    """v8 Bottleneck: cv1 (3x3) + cv2 (3x3), in that order."""
    emit_conv_block(f, bn.cv1, f"{tag}.cv1")
    emit_conv_block(f, bn.cv2, f"{tag}.cv2")


def emit_c2f(f, c2f: C2f, tag: str):
    """C2f order: cv1 (1x1), cv2 (1x1), then each Bottleneck in m[0..n-1]."""
    emit_conv_block(f, c2f.cv1, f"{tag}.cv1")
    emit_conv_block(f, c2f.cv2, f"{tag}.cv2")
    for i, bn in enumerate(c2f.m):
        emit_bottleneck(f, bn, f"{tag}.m{i}")


def emit_sppf(f, sppf: SPPF, tag: str):
    emit_conv_block(f, sppf.cv1, f"{tag}.cv1")
    emit_conv_block(f, sppf.cv2, f"{tag}.cv2")


def emit_detect(f, det: Detect, tag: str):
    """v8 anchor-free DFL head.

    For each level i in [P3, P4, P5], cv2[i] (box) is a Sequential of
    [Conv(c_in→c2, 3), Conv(c2→c2, 3), Conv2d(c2→4*reg_max, 1)] and cv3[i]
    (cls) mirrors that with c3 and nc out. We emit all box branches first,
    then all class branches — kernel matches.
    """
    for i, seq in enumerate(det.cv2):
        emit_conv_block(f, seq[0], f"{tag}.cv2[{i}].0")
        emit_conv_block(f, seq[1], f"{tag}.cv2[{i}].1")
        emit_conv(f,       seq[2], f"{tag}.cv2[{i}].2")  # nn.Conv2d, no BN
    for i, seq in enumerate(det.cv3):
        emit_conv_block(f, seq[0], f"{tag}.cv3[{i}].0")
        emit_conv_block(f, seq[1], f"{tag}.cv3[{i}].1")
        emit_conv(f,       seq[2], f"{tag}.cv3[{i}].2")  # nn.Conv2d, no BN
    # dfl.conv (16→1, fixed [0..15]) — NOT emitted; baked into the decoder.


# ---------------------------------------------------------------------------
# CRC helper
# ---------------------------------------------------------------------------
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
        f.write("/* Auto-generated by tools/export_model.py — do not edit manually */\n")
        f.write("#ifndef WEIGHTS_CRC_H\n")
        f.write("#define WEIGHTS_CRC_H\n\n")
        f.write(f"#define WEIGHTS_CRC32 0x{crc:08X}U\n")
        f.write(f"#define WEIGHTS_SIZE  {size}U\n\n")
        f.write("#endif /* WEIGHTS_CRC_H */\n")
    print(f"  weights_crc.h: CRC32=0x{crc:08X}, SIZE={size} bytes")


# ---------------------------------------------------------------------------
# Main: walk YOLOv8n graph explicitly, in inference-order
# ---------------------------------------------------------------------------
def main():
    print("=== Exporting YOLOv8n Weights (FP32 NEON-repacked) ===")
    yolo = YOLO("yolov8n.pt")
    model = yolo.model           # DetectionModel
    model.eval()
    model.fuse()                 # fold BN into Conv2d.bias / weight
    seq = model.model            # nn.Sequential of 23 blocks (0..22)

    src_dir = os.path.join(os.path.dirname(__file__), "..", "src")
    weights_path = os.path.join(src_dir, "weights.bin")

    # YOLOv8n graph (width=0.25, depth=0.33). Indices match ultralytics yaml.
    #   0  Conv      3 → 16    k=3 s=2
    #   1  Conv     16 → 32    k=3 s=2
    #   2  C2f      32 → 32    n=1  shortcut=True
    #   3  Conv     32 → 64    k=3 s=2
    #   4  C2f      64 → 64    n=2  shortcut=True
    #   5  Conv     64 → 128   k=3 s=2
    #   6  C2f     128 → 128   n=2  shortcut=True
    #   7  Conv    128 → 256   k=3 s=2
    #   8  C2f     256 → 256   n=1  shortcut=True
    #   9  SPPF    256 → 256   k=5
    #  10  Upsample
    #  11  Concat (with 6)
    #  12  C2f     384 → 128   n=1  shortcut=False
    #  13  Upsample
    #  14  Concat (with 4)
    #  15  C2f     192 → 64    n=1  shortcut=False   (→ P3 head input)
    #  16  Conv     64 → 64    k=3 s=2
    #  17  Concat (with 12)
    #  18  C2f     192 → 128   n=1  shortcut=False   (→ P4 head input)
    #  19  Conv    128 → 128   k=3 s=2
    #  20  Concat (with 9)
    #  21  C2f     384 → 256   n=1  shortcut=False   (→ P5 head input)
    #  22  Detect

    with open(weights_path, "wb") as f:
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

    print(f"\nDone. Exported to {weights_path}")

    header_path = os.path.join(src_dir, "weights_crc.h")
    write_weights_crc_header(weights_path, header_path)


if __name__ == "__main__":
    main()
