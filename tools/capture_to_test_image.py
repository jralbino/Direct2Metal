#!/usr/bin/env python3
"""
capture_to_test_image.py — turn a captured camera tensor into test_image.bin (V185).

Input: the raw file written by `make capture` (tools/hwbench.py --capture), i.e.
cam_frame exactly as the model consumed it: [3][YOLO_IN][YOLO_IN] float32, RGB
plane order, values in [0, 1], after debayer + ISP + AE.

    python3 tools/capture_to_test_image.py cam_frame.bin --preview cam_frame.png
    python3 tools/capture_to_test_image.py cam_frame.bin --no-install   # just inspect

Installs to app/<APP>/test_image.bin unless --no-install. Afterwards:
    make d2m_data.bin      # bench blob — recopy to the SD (rm → cp → sync → umount)
    make bench BENCHFLAGS=--print-golden    # paste GOLDEN / GOLDEN_ABS into hwbench.py
Needs only numpy (+ Pillow for --preview).
"""
import argparse
import os
import re
import shutil
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def yolo_in_from_bsp():
    with open(os.path.join(ROOT, "bsp", "bsp.h")) as f:
        m = re.search(r"^\s*#define\s+YOLO_IN\s+(\d+)", f.read(), re.M)
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", nargs="?", default="cam_frame.bin", help="captured tensor (default cam_frame.bin)")
    ap.add_argument("--app", default="yolo_v8n_coco")
    ap.add_argument("--size", type=int, help="N (default: YOLO_IN from bsp/bsp.h)")
    ap.add_argument("--preview", metavar="PNG", help="also write an RGB preview image")
    ap.add_argument("--no-install", action="store_true", help="inspect only; do not overwrite test_image.bin")
    args = ap.parse_args()

    n = args.size or yolo_in_from_bsp()
    if not n:
        sys.exit("cannot determine YOLO_IN; pass --size")
    want = 4 * 3 * n * n
    data = open(args.raw, "rb").read()
    if len(data) != want:
        sys.exit(f"{args.raw}: {len(data)} bytes, expected {want} (= 4*3*{n}*{n}); wrong N or truncated dump")

    t = np.frombuffer(data, dtype="<f4").reshape(3, n, n)
    lo, hi, mean = float(t.min()), float(t.max()), float(t.mean())
    print(f"tensor   [3][{n}][{n}] float32, range [{lo:.3f}, {hi:.3f}], mean {mean:.3f}")
    for c, name in enumerate("RGB"):
        print(f"  {name}: mean {t[c].mean():.3f}  std {t[c].std():.3f}")
    if not (0.0 <= lo and hi <= 1.0001):
        print("WARNING: values outside [0,1] — is this really the post-ISP cam_frame?")
    if hi - lo < 0.05:
        print("WARNING: nearly flat image (lens cap? AE not settled? raise CAPTURE=)")

    if args.preview:
        from PIL import Image
        rgb = (np.clip(t, 0, 1).transpose(1, 2, 0) * 255.0 + 0.5).astype(np.uint8)
        Image.fromarray(rgb, "RGB").save(args.preview)
        print(f"preview  {args.preview}")

    if not args.no_install:
        dst = os.path.join(ROOT, "app", args.app, "test_image.bin")
        shutil.copyfile(args.raw, dst)
        print(f"installed {dst}  ({want} bytes)")
        print("next: make d2m_data.bin  → recopy to SD →  make bench BENCHFLAGS=--print-golden  → update GOLDEN*")


if __name__ == "__main__":
    main()
