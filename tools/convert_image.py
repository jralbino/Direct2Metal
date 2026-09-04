#!/usr/bin/env python3
"""
convert_image.py — build app/<APP>/test_image.bin for the bare-metal kernel.

Output: CHW float32, [3][YOLO_H][YOLO_W], values in [0, 1], RGB plane order.
YOLO_W / YOLO_H are read from bsp/bsp.h (V192: non-square, full-FoV input) so
the tensor can never drift from what the kernel consumes.

Needs only Pillow + numpy (system python3 is fine; tools/env is not required):

    python3 tools/convert_image.py                          # bus.jpg, W×H from bsp.h
    python3 tools/convert_image.py some.jpg
    python3 tools/convert_image.py some.jpg --wh 320 192 --app yolo_v5n_coco

After regenerating: `make d2m_data.bin` (bench blob), rebuild the kernel (data.o
depends on test_image.bin), and regenerate GOLDEN / GOLDEN_ABS in tools/hwbench.py.
"""
import argparse
import os
import re
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def wh_from_bsp():
    txt = open(os.path.join(ROOT, "bsp", "bsp.h")).read()
    mw = re.search(r"^\s*#define\s+YOLO_W\s+(\d+)", txt, re.M)
    mh = re.search(r"^\s*#define\s+YOLO_H\s+(\d+)", txt, re.M)
    if mw and mh:
        return int(mw.group(1)), int(mh.group(1))
    m = re.search(r"^\s*#define\s+YOLO_IN\s+(\d+)", txt, re.M)   # legacy square
    if m:
        return int(m.group(1)), int(m.group(1))
    sys.exit("YOLO_W/YOLO_H not found in bsp/bsp.h; pass --wh")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="source image (default app/<APP>/bus.jpg)")
    ap.add_argument("--app", default="yolo_v8n_coco")
    ap.add_argument("--wh", type=int, nargs=2, metavar=("W", "H"), help="default: from bsp/bsp.h")
    ap.add_argument("--out")
    args = ap.parse_args()

    app_dir = os.path.join(ROOT, "app", args.app)
    img_path = args.image or os.path.join(app_dir, "bus.jpg")
    out_path = args.out or os.path.join(app_dir, "test_image.bin")
    w, h = tuple(args.wh) if args.wh else wh_from_bsp()

    try:
        im = Image.open(img_path).convert("RGB")
    except OSError as e:
        sys.exit(f"cannot read {img_path}: {e}")
    w0, h0 = im.size

    im = im.resize((w, h), Image.BOX)           # plain stretch to W×H
    chw = (np.asarray(im, dtype=np.float32) / 255.0).transpose(2, 0, 1)   # [3][H][W], RGB
    flat = np.ascontiguousarray(chw, dtype="<f4").ravel()

    with open(out_path, "wb") as f:
        f.write(flat.tobytes())

    print(f"source   {img_path}  ({w0}x{h0})")
    print(f"tensor   [3][{h}][{w}] float32 RGB CHW, range [{flat.min():.3f}, {flat.max():.3f}], mean {flat.mean():.3f}")
    print(f"written  {out_path}  ({flat.size * 4} bytes)")


if __name__ == "__main__":
    main()
