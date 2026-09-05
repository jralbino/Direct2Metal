"""Decode the multi-block diagnostic UART log:
  IBWP pre/post/delta + FC pre/post  → cross-frame race detector
  ROWSUM_BEGIN..ROWSUM_END           → per-row mean/max/even/odd for all 864 rows
  4× WIRE_BEGIN..WIRE_END            → 16-row hex strips at rows 50/200/400/650

Outputs:
  /tmp/rowsum.npz       — per-row arrays (mean, max, even, odd)
  /tmp/rowsum.png       — line plot of per-row mean across the whole frame
  /tmp/strip_R{NN}.bin  — raw bytes of each strip
  /tmp/strip_R{NN}.png  — debayered render of each strip (full FB_W width)

Usage: python3 tools/decode_diag.py uart.log
"""
import re
import sys
import numpy as np
from PIL import Image

WB_R, WB_G, WB_B = 572, 256, 430
BLACK = 64
RAW_STRIDE = 1920
SENSOR_BLK_W = 768
SENSOR_H = 864


def parse_strip(text, start_idx):
    """Parse one WIRE_BEGIN..WIRE_END block starting at/after start_idx.
    Returns (start_row, np.ndarray (rows, stride), end_idx) or None if no
    more blocks."""
    m = re.search(
        r"WIRE_BEGIN\s+start=([0-9A-Fa-f]+)\s+rows=([0-9A-Fa-f]+)\s+stride=([0-9A-Fa-f]+)"
        r"(.*?)WIRE_END",
        text[start_idx:], re.DOTALL,
    )
    if not m:
        return None
    start = int(m.group(1), 16)
    rows  = int(m.group(2), 16)
    stride= int(m.group(3), 16)
    body  = m.group(4)
    hex_chars = re.sub(r"[^0-9A-Fa-f]", "", body)
    expected = rows * stride * 2
    if len(hex_chars) != expected:
        raise RuntimeError(f"strip start={start} hex {len(hex_chars)} != expected {expected}")
    raw = bytes.fromhex(hex_chars)
    arr = np.frombuffer(raw, dtype=np.uint8).reshape(rows, stride)
    end_idx = start_idx + m.end()
    return start, arr, end_idx


def parse_rowsum(text):
    m = re.search(r"ROWSUM_BEGIN\s+rows=([0-9A-Fa-f]+)(.*?)ROWSUM_END", text, re.DOTALL)
    if not m:
        return None
    body = m.group(2)
    means, maxs, evens, odds = [], [], [], []
    for line in body.splitlines():
        mm = re.match(r"\s*y=([0-9A-Fa-f]+)\s+m=([0-9A-Fa-f]+)\s+X=([0-9A-Fa-f]+)\s+e=([0-9A-Fa-f]+)\s+o=([0-9A-Fa-f]+)", line)
        if not mm:
            continue
        means.append(int(mm.group(2), 16))
        maxs.append(int(mm.group(3), 16))
        evens.append(int(mm.group(4), 16))
        odds.append(int(mm.group(5), 16))
    return (np.array(means, np.uint8), np.array(maxs, np.uint8),
            np.array(evens, np.uint8), np.array(odds, np.uint8))


def render_strip(strip):
    rows, stride = strip.shape
    fb_w = 1920
    out_h = rows * 4
    out = np.zeros((out_h, fb_w, 3), dtype=np.uint8)
    pedestal = BLACK >> 2
    src8 = (strip.astype(np.int16) - pedestal).clip(0).astype(np.uint16)
    for vy in range(out_h):
        local = vy // 4
        sy_even = (local // 2) * 2
        if sy_even + 1 >= rows:
            sy_even = rows - 2
        row_e = src8[sy_even]
        row_o = src8[sy_even + 1] if sy_even + 1 < rows else row_e
        for fx in range(fb_w):
            blk_x = (SENSOR_BLK_W - 1) - (fx * SENSOR_BLK_W) // fb_w
            sx_even = blk_x * 2
            R  = int(row_e[(sx_even >> 2) * 5 + (sx_even & 3)])
            Gr = int(row_e[(sx_even >> 2) * 5 + ((sx_even + 1) & 3)])
            Gb = int(row_o[(sx_even >> 2) * 5 + (sx_even & 3)])
            B  = int(row_o[(sx_even >> 2) * 5 + ((sx_even + 1) & 3)])
            G  = (Gr + Gb) >> 1
            out[vy, fx, 0] = min(255, (R * WB_R) >> 8)
            out[vy, fx, 1] = min(255, (G * WB_G) >> 8)
            out[vy, fx, 2] = min(255, (B * WB_B) >> 8)
    return out


def plot_rowsum(means, maxs, evens, odds, path):
    """Render a 4-channel per-row line plot as a PNG without matplotlib."""
    H, W = 320, max(SENSOR_H, 864)
    img = np.full((H, W, 3), 255, np.uint8)
    grid_y = [0, 64, 128, 192, 256, 319]
    for gy in grid_y:
        img[gy, :, :] = 200
    def draw(arr, color):
        for x in range(min(W, len(arr))):
            y = H - 1 - int(int(arr[x]) * (H - 1) / 255)
            y = max(0, min(H - 1, y))
            img[y, x] = color
    draw(means, [0, 0, 0])
    draw(maxs,  [255, 0, 0])
    draw(evens, [0, 128, 0])
    draw(odds,  [0, 0, 255])
    Image.fromarray(img).save(path)


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "uart.log"
    text = open(path, "r", errors="ignore").read()

    # Race detector
    m = re.search(
        r"IBWP pre =0x([0-9A-Fa-f]+)\s+post=0x([0-9A-Fa-f]+)\s+delta=0x([0-9A-Fa-f]+)",
        text)
    if m:
        pre, post, delta = (int(m.group(i), 16) for i in (1, 2, 3))
        print(f"IBWP pre =0x{pre:08X} post=0x{post:08X} delta=0x{delta:X}")
        if delta != 0:
            print("  ⚠ DMA advanced during snapshot → cross-frame stitching is real")
        else:
            print("  ✓ DMA frozen during snapshot")
    m = re.search(r"FC\s+pre =0x([0-9A-Fa-f]+)\s+post=0x([0-9A-Fa-f]+)", text)
    if m:
        fp, fq = int(m.group(1), 16), int(m.group(2), 16)
        print(f"FrameCount pre={fp} post={fq}"
              + ("  ⚠ frame counter ticked during snapshot" if fp != fq
                 else "  ✓ same frame both reads"))

    # Row summary
    rs = parse_rowsum(text)
    if rs is not None:
        means, maxs, evens, odds = rs
        np.savez("/tmp/rowsum.npz", mean=means, max=maxs, even=evens, odd=odds)
        plot_rowsum(means, maxs, evens, odds, "/tmp/rowsum.png")
        # Surface anomalous rows: |row mean − median of ±5 neighbours of same parity| > 30
        anom = []
        for y in range(SENSOR_H):
            same_par = means[max(0, y - 10):y + 11:2]
            if len(same_par) < 3:
                continue
            med = int(np.median(same_par))
            if abs(int(means[y]) - med) > 25 and means[y] > 4:
                anom.append((y, int(means[y]), med))
        print(f"\nRow-summary loaded: {SENSOR_H} rows.")
        print(f"Anomalous rows (mean off by >25 from same-parity neighbours):")
        for y, m_, med in anom[:60]:
            print(f"  y={y:4d}  mean={m_:3d}  neighbours_med={med:3d}  delta={m_-med:+4d}")
        if len(anom) > 60:
            print(f"  ... +{len(anom) - 60} more")

    # Strips
    idx = 0
    n = 0
    while True:
        result = parse_strip(text, idx)
        if result is None:
            break
        start_row, strip, idx = result
        binp = f"/tmp/strip_R{start_row:03d}.bin"
        pngp = f"/tmp/strip_R{start_row:03d}.png"
        with open(binp, "wb") as f:
            f.write(strip.tobytes())
        Image.fromarray(render_strip(strip)).save(pngp)
        print(f"strip @ row {start_row:3d}: mean={strip.mean():.1f}  -> {binp} {pngp}")
        n += 1
    print(f"decoded {n} strips")
