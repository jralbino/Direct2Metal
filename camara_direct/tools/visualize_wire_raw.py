"""Visualize wire bytes as plain grayscale heatmaps so structure is visible
without any debayer interpretation:
  /tmp/wire_raw_packed.png  — 16 × 1920 bytes as-is (RAW10 packed)
  /tmp/wire_raw_unpacked.png — 16 × 1536 unpacked MSB8 pixels (4-of-5 bytes
                                kept; LSB pack byte stripped)
  /tmp/wire_raw_unpacked_2x.png — 32 × 1536 vertically doubled for clarity

Each row is rendered as one pixel-row. Real-scene rows from a static target
should look like nearly identical horizontal streaks. A horizontal SHIFT
between adjacent rows indicates D-PHY skew or DMA stride error. A complete
content change between adjacent rows indicates row-swap / buffer-address bug.
"""
import numpy as np
from PIL import Image

RAW_STRIDE = 1920
ROWS = 16

raw = np.frombuffer(open("/tmp/wire_strip.bin", "rb").read(), np.uint8)
strip = raw.reshape(ROWS, RAW_STRIDE)

# Packed view: every 5th byte is the LSB pack which is structurally
# different from MSB bytes — visible as periodic vertical noise stripes
Image.fromarray(strip, mode="L").resize((1920, 16 * 8), Image.NEAREST).save(
    "/tmp/wire_raw_packed.png")

# Unpacked: keep only MSB8 bytes (4 out of every 5), giving 1536 pixels/row
def unpack(row):
    out = np.empty(1536, dtype=np.uint8)
    for g in range(384):
        out[g * 4:(g + 1) * 4] = row[g * 5:g * 5 + 4]
    return out

unpacked = np.stack([unpack(strip[y]) for y in range(ROWS)])
Image.fromarray(unpacked, mode="L").resize((1536, 16 * 8), Image.NEAREST).save(
    "/tmp/wire_raw_unpacked.png")

# Even-rows-only (R/Gr) and odd-rows-only (Gb/B) so Bayer colour channels
# don't create a striped pattern that hides real row-shift
even = unpacked[0::2]   # 8 rows × 1536
odd  = unpacked[1::2]
Image.fromarray(even, mode="L").resize((1536, 8 * 16), Image.NEAREST).save(
    "/tmp/wire_raw_even_rows.png")
Image.fromarray(odd, mode="L").resize((1536, 8 * 16), Image.NEAREST).save(
    "/tmp/wire_raw_odd_rows.png")

print("wrote /tmp/wire_raw_*.png")
print(f"strip min={strip.min()} max={strip.max()} mean={strip.mean():.1f}")
print(f"even-rows min={even.min()} max={even.max()} mean={even.mean():.1f}")
print(f"odd-rows  min={odd.min()} max={odd.max()} mean={odd.mean():.1f}")
