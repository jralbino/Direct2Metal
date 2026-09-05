"""Decode the WIRE_BEGIN..WIRE_END block emitted by main.cpp::dump_wire_strip
and render those 16 wire rows through the firmware's debayer math.

Usage:
    python3 tools/decode_wire_strip.py uart_capture.txt

Produces:
    /tmp/wire_strip.bin     — raw 16*1920 bytes
    /tmp/wire_strip.png     — debayered + WB + 180° flip + 5/4 horizontal
                              upscale, vertically just those 16 rows × 2.5

If wire_strip.png shows the same horizontal "line swap" the user sees on
HDMI, the bug is in the wire bytes (D-PHY, stride, snapshot race). If
wire_strip.png is clean, the bug is in display rendering on the Pi.
"""
import re
import sys
import numpy as np
from PIL import Image

WB_R, WB_G, WB_B = 572, 256, 430
BLACK = 64
RAW_STRIDE = 1920
SENSOR_BLK_W = 768


def parse_capture(path: str):
    text = open(path, "r", errors="ignore").read()
    m = re.search(
        r"WIRE_BEGIN\s+start=([0-9A-Fa-f]+)\s+rows=([0-9A-Fa-f]+)\s+stride=([0-9A-Fa-f]+)"
        r"(.*?)WIRE_END",
        text, re.DOTALL,
    )
    if not m:
        raise RuntimeError("WIRE_BEGIN/WIRE_END markers not found")
    start = int(m.group(1), 16)
    rows  = int(m.group(2), 16)
    stride= int(m.group(3), 16)
    body  = m.group(4)
    hex_chars = re.sub(r"[^0-9A-Fa-f]", "", body)
    expected = rows * stride * 2
    if len(hex_chars) != expected:
        raise RuntimeError(f"hex length {len(hex_chars)} != expected {expected}")
    raw = bytes.fromhex(hex_chars)
    return start, rows, stride, np.frombuffer(raw, dtype=np.uint8).reshape(rows, stride)


def msb8(row: np.ndarray, col: int) -> int:
    return int(row[(col >> 2) * 5 + (col & 3)])


def render_strip(strip: np.ndarray) -> np.ndarray:
    rows, stride = strip.shape
    fb_w = 1920
    out_h = rows * 2  # render each Bayer block as 2 rows for visibility
    out = np.zeros((out_h, fb_w, 3), dtype=np.uint8)

    pedestal = BLACK >> 2
    src8 = (strip.astype(np.int16) - pedestal).clip(0).astype(np.uint16)

    for vy in range(out_h):
        sy_even = (vy // 2) * 2 if (vy // 2) * 2 + 1 < rows else (rows - 2)
        if sy_even % 2 != 0:
            sy_even -= 1  # ensure even row in strip-local coords
        row_e = src8[sy_even]
        row_o = src8[sy_even + 1] if sy_even + 1 < rows else row_e

        for fx in range(fb_w):
            blk_x = (SENSOR_BLK_W - 1) - (fx * SENSOR_BLK_W) // fb_w
            sx_even = blk_x * 2

            R  = row_e[(sx_even >> 2) * 5 + (sx_even & 3)]
            Gr = row_e[(sx_even >> 2) * 5 + ((sx_even + 1) & 3)]
            Gb = row_o[(sx_even >> 2) * 5 + (sx_even & 3)]
            B  = row_o[(sx_even >> 2) * 5 + ((sx_even + 1) & 3)]
            G = (Gr + Gb) >> 1

            out[vy, fx, 0] = min(255, (R * WB_R) >> 8)
            out[vy, fx, 1] = min(255, (G * WB_G) >> 8)
            out[vy, fx, 2] = min(255, (B * WB_B) >> 8)
    return out


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "uart_capture.txt"
    start, rows, stride, strip = parse_capture(path)
    print(f"decoded {rows} rows × {stride} bytes starting at row {start}")
    with open("/tmp/wire_strip.bin", "wb") as f:
        f.write(strip.tobytes())
    img = render_strip(strip)
    Image.fromarray(img).save("/tmp/wire_strip.png")
    print("wrote /tmp/wire_strip.bin and /tmp/wire_strip.png")
