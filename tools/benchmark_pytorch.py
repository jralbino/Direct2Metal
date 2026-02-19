"""
tools/benchmark_pytorch.py — Standard baseline benchmark for comparison.

Run this on a Raspberry Pi Zero 2 W with Raspberry Pi OS + Python:
  pip install torch --index-url https://download.pytorch.org/whl/cpu
  python3 benchmark_pytorch.py

This measures the SAME TinyYOLO model inference time using the standard
Python + PyTorch stack, to compare against our bare-metal implementation.

Usage:
  python3 tools/benchmark_pytorch.py           # default: 20 warmup + 50 runs
  python3 tools/benchmark_pytorch.py --runs 10 # quick test
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
import time
import argparse
import sys

# ---------------------------------------------------------------------------
# Network (identical architecture to bare-metal kernel)
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
        return self.head(x)

# ---------------------------------------------------------------------------
# Layer-by-layer timing (same breakdown as bare-metal UART output)
# ---------------------------------------------------------------------------

def benchmark_layerwise(model, img, runs=20):
    """Measure each layer separately to match bare-metal benchmark format."""
    timings = {'L1': [], 'L2': [], 'L3': [], 'Head': [], 'Total': []}

    with torch.no_grad():
        for _ in range(runs):
            t0 = time.perf_counter()

            t_l1s = time.perf_counter()
            x = F.leaky_relu(model.bn1(model.conv1(img)), negative_slope=0.1)
            t_l1e = time.perf_counter()

            t_l2s = time.perf_counter()
            x = F.leaky_relu(model.bn2(model.conv2(x)), negative_slope=0.1)
            t_l2e = time.perf_counter()

            t_l3s = time.perf_counter()
            x = F.leaky_relu(model.bn3(model.conv3(x)), negative_slope=0.1)
            t_l3e = time.perf_counter()

            t_hs = time.perf_counter()
            x = model.pool(x)
            x = model.head(x)
            t_he = time.perf_counter()

            t1 = time.perf_counter()

            timings['L1'].append((t_l1e - t_l1s) * 1000)
            timings['L2'].append((t_l2e - t_l2s) * 1000)
            timings['L3'].append((t_l3e - t_l3s) * 1000)
            timings['Head'].append((t_he  - t_hs)  * 1000)
            timings['Total'].append((t1 - t0) * 1000)

    # Drop first few runs (JIT warmup effects)
    skip = min(5, runs // 4)
    return {k: sorted(v[skip:])[len(v[skip:])//2] for k, v in timings.items()}  # median

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runs',   type=int, default=50, help='Number of timed runs')
    parser.add_argument('--warmup', type=int, default=20, help='Warmup runs (not timed)')
    parser.add_argument('--threads', type=int, default=1, help='PyTorch intra-op threads')
    args = parser.parse_args()

    torch.set_num_threads(args.threads)
    torch.set_num_interop_threads(1)

    print("=" * 50)
    print("  TinyYOLO PyTorch Baseline Benchmark")
    print("  (Standard implementation — no bare-metal)")
    print("=" * 50)
    print(f"PyTorch version : {torch.__version__}")
    print(f"Threads         : {args.threads}")
    print(f"Runs            : {args.runs} (+ {args.warmup} warmup)")
    print()

    torch.manual_seed(7)
    model = TinyYOLO()
    model.eval()

    # Match the same test image as bare-metal
    img = torch.rand(1, 3, 64, 64)

    # Warmup
    print("Warming up...", end='', flush=True)
    with torch.no_grad():
        for _ in range(args.warmup):
            _ = model(img)
    print(" done.")
    print()

    # Timed runs
    print("Timing...", flush=True)
    t = benchmark_layerwise(model, img, runs=args.runs)

    print()
    print("=== PERFORMANCE (median over timed runs) ===")
    print(f"L1 (Conv 3->16):   {t['L1']:7.1f} ms")
    print(f"L2 (Conv 16->32):  {t['L2']:7.1f} ms")
    print(f"L3 (Conv 32->64):  {t['L3']:7.1f} ms")
    print(f"Head + Decode:     {t['Head']:7.1f} ms")
    print("-" * 30)
    print(f"TOTAL TIME:        {t['Total']:7.1f} ms")
    fps = 1000.0 / t['Total'] if t['Total'] > 0 else 0
    print(f"FPS:               {fps:7.1f}")
    print()

    # -----------------------------------------------------------------------
    # Comparison table against bare-metal results
    # -----------------------------------------------------------------------
    print("=" * 60)
    print("  COMPARISON TABLE vs Bare-Metal Direct2Metal")
    print("=" * 60)
    print(f"{'Implementation':<35} {'Latency':>10} {'FPS':>6}")
    print("-" * 60)

    # Bare-metal measured results (update these after each HW run)
    # These are the actual numbers measured on RPi Zero 2 W real silicon
    rows = [
        ("Linux + Python + PyTorch (this run)",         t['Total'], fps),
        ("Bare Metal: C++ scalar (no cache)",           6900.0,     0.14),
        ("Bare Metal: C++ scalar + cache/MMU",           222.0,     4.5),
        ("Bare Metal: NEON 4ch + cache/MMU [target]",   None,       None),
    ]

    for name, ms, fp in rows:
        if ms is None:
            print(f"  {name:<33} {'[run on HW]':>10} {'[run on HW]':>10}")
        else:
            print(f"  {name:<33} {ms:>8.0f}ms {fp:>7.1f}")

    print()
    if t['Total'] > 0:
        speedup_vs_scalar = t['Total'] / 222.0
        print(f"  Bare-metal (with cache) is {speedup_vs_scalar:.1f}x faster than PyTorch baseline")
        print(f"  (Once NEON 4ch results from HW are available, add them above)")

    print()
    print("HOW TO CAPTURE BARE-METAL NUMBERS:")
    print("  1. Flash kernel8.img to SD card")
    print("  2. Connect serial at 115200 baud")
    print("  3. Read 'TOTAL TIME' from UART output")
    print("  4. Update the table above with real measurements")

if __name__ == '__main__':
    main()
