#!/usr/bin/env python3
"""
convert_image.py — build app/<APP>/test_image.bin for the bare-metal kernel.

Output: CHW float32, [3][N][N], values in [0, 1], RGB plane order, where N is
the model input side. N is read from bsp/bsp.h (`#define YOLO_IN N`) so the
tensor can never drift from what the kernel consumes — the V169..V183 fossil
was a 320² tensor fed to a 256² model, which silently misaligned the colour
planes (the kernel reads the first 3*N*N floats of a 3*320*320 buffer).

Needs only Pillow + numpy (system python3 is fine; tools/env is not required):

    python3 tools/convert_image.py                          # bus.jpg, N from bsp.h
    python3 tools/convert_image.py some.jpg                 # other image
    python3 tools/convert_image.py some.jpg --size 320 --app yolo_v5n_coco

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


def yolo_in_from_bsp():
    hdr = os.path.join(ROOT, "bsp", "bsp.h")
    with open(hdr) as f:
        m = re.search(r"^\s*#define\s+YOLO_IN\s+(\d+)", f.read(), re.M)
    if not m:
        sys.exit(f"YOLO_IN not found in {hdr}; pass --size")
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="source image (default app/<APP>/bus.jpg)")
    ap.add_argument("--app", default="yolo_v8n_coco", help="app folder under app/")
    ap.add_argument("--size", type=int, help="input side N (default: YOLO_IN from bsp/bsp.h)")
    ap.add_argument("--out", help="output path (default app/<APP>/test_image.bin)")
    args = ap.parse_args()

    app_dir = os.path.join(ROOT, "app", args.app)
    img_path = args.image or os.path.join(app_dir, "bus.jpg")
    out_path = args.out or os.path.join(app_dir, "test_image.bin")
    n = args.size or yolo_in_from_bsp()

    try:
        im = Image.open(img_path).convert("RGB")
    except OSError as e:
        sys.exit(f"cannot read {img_path}: {e}")
    w0, h0 = im.size

    # Plain resize to N×N (no letterbox) — matches the kernel's camera crop path
    # and what export/calibration assumed. BOX ≈ cv2 INTER_AREA.
    im = im.resize((n, n), Image.BOX)
    chw = (np.asarray(im, dtype=np.float32) / 255.0).transpose(2, 0, 1)   # [3][N][N], RGB
    flat = np.ascontiguousarray(chw, dtype="<f4").ravel()

    with open(out_path, "wb") as f:
        f.write(flat.tobytes())

    print(f"source   {img_path}  ({w0}x{h0})")
    print(f"tensor   [3][{n}][{n}] float32 RGB CHW, range [{flat.min():.3f}, {flat.max():.3f}], mean {flat.mean():.3f}")
    print(f"written  {out_path}  ({flat.size * 4} bytes)")


if __name__ == "__main__":
    main()
