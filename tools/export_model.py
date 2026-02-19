"""
tools/export_model.py — TinyYOLO weight exporter for bare-metal NEON inference.

Weight layout in weights.bin — NEON-repacked conv weights:
  conv1 repacked  [C_out/4][C_in*K*K][4] = [4][27][4]   = 432 floats
  bn1 pre-fused   [scale(16), bias(16)]                  = 32 floats
  conv2 repacked  [C_out/4][C_in*K*K][4] = [8][144][4]  = 4608 floats
  bn2 pre-fused   [scale(32), bias(32)]                  = 64 floats
  conv3 repacked  [C_out/4][C_in*K*K][4] = [16][288][4] = 18432 floats
  bn3 pre-fused   [scale(64), bias(64)]                  = 128 floats
  head.weight     [6, 64]                                = 384 floats
  head.bias       [6]                                    = 6 floats
  TOTAL: 24086 floats = 96344 bytes  (same size, different layout)

NEON repacking: for a 4-channel group g and inner position p = ci*K*K + ky*K + kx,
  repacked[g * C_in*K*K + p] = [w[g*4, ci, ky, kx],
                                  w[g*4+1, ci, ky, kx],
                                  w[g*4+2, ci, ky, kx],
                                  w[g*4+3, ci, ky, kx]]
These 4 values are contiguous → single vld1q_f32 instruction in NEON.
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
import numpy as np
import struct, os

torch.manual_seed(42)

# ---------------------------------------------------------------------------
# 1. Network definition
# ---------------------------------------------------------------------------

class TinyYOLO(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(3,  16, 3, stride=1, padding=1, bias=False)
        self.bn1   = nn.BatchNorm2d(16)
        self.conv2 = nn.Conv2d(16, 32, 3, stride=2, padding=1, bias=False)
        self.bn2   = nn.BatchNorm2d(32)
        self.conv3 = nn.Conv2d(32, 64, 3, stride=2, padding=1, bias=False)
        self.bn3   = nn.BatchNorm2d(64)
        self.pool  = nn.MaxPool2d(2, 2)
        self.head  = nn.Conv2d(64,  6, 1, stride=1, padding=0, bias=True)

    def forward(self, x):
        x = F.leaky_relu(self.bn1(self.conv1(x)), negative_slope=0.1)
        x = F.leaky_relu(self.bn2(self.conv2(x)), negative_slope=0.1)
        x = F.leaky_relu(self.bn3(self.conv3(x)), negative_slope=0.1)
        x = self.pool(x)
        x = self.head(x)
        return x

# ---------------------------------------------------------------------------
# 2. BN fusion
# ---------------------------------------------------------------------------

def fuse_bn(bn_layer, eps=1e-5):
    gamma = bn_layer.weight.detach().numpy()
    beta  = bn_layer.bias.detach().numpy()
    mean  = bn_layer.running_mean.detach().numpy()
    var   = bn_layer.running_var.detach().numpy()
    scale = gamma / (var + eps) ** 0.5
    bias  = beta - mean * scale
    return scale.astype('float32'), bias.astype('float32')

# ---------------------------------------------------------------------------
# 3. NEON weight repacking
#
# Input:  numpy array [C_out, C_in, K, K]
# Output: numpy array [C_out/4 * C_in*K*K * 4]  (flat)
#
# For each 4-channel group g and kernel position (ci, ky, kx):
#   output[g * C_in*K*K + ci*K*K + ky*K + kx][0..3]
#     = w[g*4+0..3, ci, ky, kx]
#
# This layout lets the NEON kernel do one vld1q_f32 per (ci,ky,kx) step
# to load all 4 output-channel weights simultaneously.
# ---------------------------------------------------------------------------

def repack_for_neon(weight_np):
    """
    weight_np: [C_out, C_in, K, K] float32 array
    returns:   [C_out_pad/4, C_in*K*K, 4] flat float32 array
               where C_out_pad = ceil(C_out/4)*4 (zero-padded)
    """
    C_out, C_in, K, _ = weight_np.shape
    C_out_pad = ((C_out + 3) // 4) * 4

    # Flatten spatial: [C_out, C_in*K*K]
    w = weight_np.reshape(C_out, C_in * K * K).astype('float32')

    # Zero-pad C_out to multiple of 4
    if C_out_pad > C_out:
        w = np.concatenate([w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype='float32')], axis=0)

    # Reshape to [C_out_pad/4, 4, C_in*K*K]
    w = w.reshape(C_out_pad // 4, 4, C_in * K * K)

    # Transpose to [C_out_pad/4, C_in*K*K, 4] → contiguous weights per position
    w = w.transpose(0, 2, 1)  # [groups, inner, 4]

    n_floats = (C_out_pad // 4) * (C_in * K * K) * 4
    assert n_floats == C_out_pad * C_in * K * K
    # Verify same total count (C_out padded)
    flat = w.flatten()
    # Trim to C_out * C_in * K * K (drop zero-pad groups if any)
    # Actually for our network C_out is always divisible by 4, so no trimming needed
    return flat

# ---------------------------------------------------------------------------
# 4. Export weights.bin
# ---------------------------------------------------------------------------

def export_weights(model, path):
    floats = []

    def add_conv_neon(layer, label):
        w_np = layer.weight.detach().numpy()
        repacked = repack_for_neon(w_np)
        floats.extend(repacked)
        print(f"  {label} conv (repacked): {len(repacked)} floats")
        return len(repacked)

    def add_bn(bn_layer, label):
        s, b = fuse_bn(bn_layer)
        floats.extend(s)
        floats.extend(b)
        n = len(s) + len(b)
        print(f"  {label} BN:              {n} floats  (scale+bias)")
        return n

    print("=== Exporting TinyYOLO (NEON repacked) ===")

    offsets = {}

    offsets['conv1'] = len(floats)
    add_conv_neon(model.conv1, 'L1')
    offsets['bn1'] = len(floats)
    add_bn(model.bn1, 'L1')

    offsets['conv2'] = len(floats)
    add_conv_neon(model.conv2, 'L2')
    offsets['bn2'] = len(floats)
    add_bn(model.bn2, 'L2')

    offsets['conv3'] = len(floats)
    add_conv_neon(model.conv3, 'L3')
    offsets['bn3'] = len(floats)
    add_bn(model.bn3, 'L3')

    offsets['head_w'] = len(floats)
    wh = model.head.weight.detach().numpy().flatten()
    bh = model.head.bias.detach().numpy().flatten()
    floats.extend(wh)
    offsets['head_b'] = len(floats)
    floats.extend(bh)
    print(f"  Head conv:              {len(wh)} floats")
    print(f"  Head bias:              {len(bh)} floats")

    total = len(floats)
    print(f"  TOTAL: {total} floats = {total*4} bytes")

    with open(path, 'wb') as f:
        f.write(struct.pack(f'{total}f', *floats))
    print(f"  Written: {path}")
    return offsets

# ---------------------------------------------------------------------------
# 5. Export test_image.bin  (CHW float32)
# ---------------------------------------------------------------------------

def export_test_image(path):
    torch.manual_seed(7)
    img = torch.rand(1, 3, 64, 64)
    arr = img[0].numpy().flatten()
    with open(path, 'wb') as f:
        f.write(struct.pack(f'{len(arr)}f', *arr))
    print(f"  Test image: {len(arr)} floats → {path}")
    return img

# ---------------------------------------------------------------------------
# 6. Export reference output
# ---------------------------------------------------------------------------

def export_reference(model, img, path):
    model.eval()
    with torch.no_grad():
        out = model(img)
    arr = out[0].numpy().flatten()
    with open(path, 'w') as f:
        for v in arr:
            f.write(f'{v:.8f}\n')
    print(f"  Reference: {len(arr)} values → {path}")
    print("  First 10 head_out values:")
    for i, v in enumerate(arr[:10]):
        print(f"    [{i}] = {v:.6f}")
    return arr

# ---------------------------------------------------------------------------
# 7. Generate model_config.h
# ---------------------------------------------------------------------------

def export_config(offsets, path):
    lines = [
        "/* AUTO-GENERATED by tools/export_model.py — do not edit manually */",
        "#ifndef MODEL_CONFIG_H",
        "#define MODEL_CONFIG_H",
        "",
        "/* Input */",
        "#define INPUT_H     64",
        "#define INPUT_W     64",
        "#define INPUT_C     3",
        "",
        "/* Layer 1: Conv2d(3->16, 3x3, s=1, p=1) + BN */",
        "#define L1_IN_CH    3",
        "#define L1_OUT_CH   16",
        "#define L1_KER_SZ   3",
        "#define L1_STRIDE   1",
        "#define L1_PAD      1",
        "#define L1_OUT_H    64",
        "#define L1_OUT_W    64",
        "",
        "/* Layer 2: Conv2d(16->32, 3x3, s=2, p=1) + BN */",
        "#define L2_IN_CH    16",
        "#define L2_OUT_CH   32",
        "#define L2_KER_SZ   3",
        "#define L2_STRIDE   2",
        "#define L2_PAD      1",
        "#define L2_OUT_H    32",
        "#define L2_OUT_W    32",
        "",
        "/* Layer 3: Conv2d(32->64, 3x3, s=2, p=1) + BN */",
        "#define L3_IN_CH    32",
        "#define L3_OUT_CH   64",
        "#define L3_KER_SZ   3",
        "#define L3_STRIDE   2",
        "#define L3_PAD      1",
        "#define L3_OUT_H    16",
        "#define L3_OUT_W    16",
        "",
        "/* MaxPool 2x2 s=2 */",
        "#define POOL_OUT_H  8",
        "#define POOL_OUT_W  8",
        "",
        "/* YOLO head: Conv1x1(64->6) */",
        "#define HEAD_IN_CH  64",
        "#define HEAD_OUT_CH 6",
        "#define GRID_H      8",
        "#define GRID_W      8",
        "",
        "/* Weight offsets (in floats from weights_start) */",
        f"#define W_CONV1_OFF  {offsets['conv1']}",
        f"#define W_BN1_OFF    {offsets['bn1']}",
        f"#define W_CONV2_OFF  {offsets['conv2']}",
        f"#define W_BN2_OFF    {offsets['bn2']}",
        f"#define W_CONV3_OFF  {offsets['conv3']}",
        f"#define W_BN3_OFF    {offsets['bn3']}",
        f"#define W_HEAD_W_OFF {offsets['head_w']}",
        f"#define W_HEAD_B_OFF {offsets['head_b']}",
        "",
        "#define TOTAL_WEIGHTS_SIZE 96344  /* bytes, unchanged */",
        "",
        "#endif /* MODEL_CONFIG_H */",
    ]
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"  Config: {path}")

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    src_dir  = os.path.join(os.path.dirname(__file__), '..', 'src')
    tool_dir = os.path.dirname(__file__)

    model = TinyYOLO()
    model.train()
    with torch.no_grad():
        _ = model(torch.rand(1, 3, 64, 64))   # populate BN running stats
    model.eval()

    weights_path = os.path.join(src_dir,  'weights.bin')
    image_path   = os.path.join(src_dir,  'test_image.bin')
    ref_path     = os.path.join(tool_dir, 'reference_output.txt')
    config_path  = os.path.join(src_dir,  'model_config.h')

    offsets = export_weights(model, weights_path)
    img     = export_test_image(image_path)
    _       = export_reference(model, img, ref_path)
    export_config(offsets, config_path)

    print("\nDone. Rebuild the kernel to use NEON conv2d.")
