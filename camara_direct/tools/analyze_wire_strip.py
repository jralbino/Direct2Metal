"""Statistical analysis of /tmp/wire_strip.bin (16 rows × 1920 bytes RAW10 packed).

Looks for the kind of structural anomalies that would manifest as visual
"line swap" on HDMI:
  - per-row mean/min/max divergence between adjacent same-parity rows
  - even/odd row Bayer-channel separation (R vs Gr; Gb vs B)
  - horizontal byte-position drift between adjacent rows (xcorr peak shift)
"""
import numpy as np

RAW_STRIDE = 1920
ROWS = 16
BLACK = 64

raw = np.frombuffer(open("/tmp/wire_strip.bin", "rb").read(), dtype=np.uint8)
strip = raw.reshape(ROWS, RAW_STRIDE)
print(f"strip shape: {strip.shape}  global mean={strip.mean():.1f}")

# Unpack RAW10 → 1536 MSB8 pixels per row (skip every 5th LSB byte)
def unpack_msb8(row):
    out = np.empty(1536, dtype=np.uint8)
    for g in range(384):
        for k in range(4):
            out[g * 4 + k] = row[g * 5 + k]
    return out

print("\nrow#  parity  mean  min  max  | bayer R  Gr  Gb  B  (channel means)")
for y in range(ROWS):
    row = strip[y]
    parity = "EVEN" if y % 2 == 0 else "ODD "
    p = unpack_msb8(row).astype(np.int16)
    p = (p - (BLACK >> 2)).clip(0)
    # Even row: pixels (0,2,4...) are R or Gb depending on row parity
    #          pixels (1,3,5...) are Gr or B
    # Within strip starting row 100 (even sensor row in original frame), so
    # strip row 0 = R/Gr, strip row 1 = Gb/B, etc.
    even_cols = p[0::2]
    odd_cols  = p[1::2]
    print(f"{y:3d}    {parity}    {row.mean():5.1f} {row.min():3d}  {row.max():3d} | "
          f"col_even={even_cols.mean():5.1f}  col_odd={odd_cols.mean():5.1f}")

# Per-row signature: dot product with a fixed reference, looks for shift
print("\nCross-correlation between adjacent same-parity rows (peak offset):")
def best_shift(a, b, max_shift=20):
    """Find shift s that maximises sum(a[s:] * b[:len(a)-s]) — i.e. how much
    row b is offset to the right of row a."""
    best = (0, -1.0)
    a = a.astype(np.float64) - a.mean()
    b = b.astype(np.float64) - b.mean()
    n = len(a)
    norm = np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9
    for s in range(-max_shift, max_shift + 1):
        if s >= 0:
            v = (a[s:] * b[:n - s]).sum() / norm
        else:
            v = (a[:n + s] * b[-s:]).sum() / norm
        if v > best[1]:
            best = (s, v)
    return best

for y in range(0, ROWS - 2, 1):
    a = unpack_msb8(strip[y])
    b = unpack_msb8(strip[y + 2])  # skip 1 row → same Bayer parity
    shift, score = best_shift(a, b)
    print(f"row {y:3d} -> row {y+2:3d} (same parity): peak shift={shift:+3d} pixels  score={score:.3f}")
