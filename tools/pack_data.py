#!/usr/bin/env python3
"""
pack_data.py — build d2m_data.bin for the serial-boot bench (V183).

  d2m_data.bin = weights.bin  (FP32 blob, WEIGHTS_SIZE bytes)
               + zero pad to a 16-byte boundary
               + test_image.bin

The VPU loads this file to D2M_BLOB_ADDR (0x08000000) via the `initramfs`
line in build/config.serial.txt. The -DSERIAL_BOOT build of yolo_v8n.cpp
points weights_start / weights_end / test_image into it, computing the
test_image offset from WEIGHTS_SIZE exactly as this script does — keep the
two in sync.

Only the FP32 blob is packed: the bench is fp32-only (SERIAL_BOOT + USE_INT8*
is a build error).

Pure python, no torch — runs natively or in the rpi-forge container.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app", "yolo_v8n_coco")
W_PATH = os.path.join(APP, "weights.bin")
IMG_PATH = os.path.join(APP, "test_image.bin")
OUT_PATH = os.path.join(ROOT, "d2m_data.bin")
CRC_H = os.path.join(APP, "weights_crc.h")

ALIGN = 16
MAX_BLOB = 256 * 1024 * 1024   # must stay below 0x18000000 - 0x08000000


def read(path):
    with open(path, "rb") as f:
        return f.read()


def weights_size_from_header():
    """Cross-check against WEIGHTS_SIZE in weights_crc.h (export_model.py)."""
    try:
        for line in open(CRC_H):
            if line.startswith("#define WEIGHTS_SIZE"):
                return int(line.split()[2].rstrip("U"))
    except OSError:
        pass
    return None


def main():
    w = read(W_PATH)
    img = read(IMG_PATH)
    hdr = weights_size_from_header()
    if hdr is not None and hdr != len(w):
        sys.exit(f"weights.bin is {len(w)} B but weights_crc.h says WEIGHTS_SIZE={hdr} "
                 f"— re-run tools/export_model.py")
    pad = (-len(w)) % ALIGN
    blob = w + b"\x00" * pad + img
    if len(blob) > MAX_BLOB:
        sys.exit(f"blob {len(blob)} B exceeds {MAX_BLOB} B")
    with open(OUT_PATH, "wb") as f:
        f.write(blob)
    print(f"weights     {len(w):>10} B")
    print(f"pad         {pad:>10} B")
    print(f"test_image  {len(img):>10} B  @ offset {len(w) + pad} (0x{len(w) + pad:08X})")
    print(f"d2m_data.bin {len(blob):>9} B  -> {OUT_PATH}")


if __name__ == "__main__":
    main()
