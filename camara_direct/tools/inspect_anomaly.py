"""Compare rows 5 (clean), 7 (anomalous), 9 (anomalous), 11 (partial)
byte-by-byte. Look for: leading zero gap (D-PHY drop), period of repeat
(stride drift), or specific column where divergence starts."""
import numpy as np

RAW_STRIDE = 1920
strip = np.frombuffer(open("/tmp/wire_strip.bin", "rb").read(), np.uint8).reshape(16, RAW_STRIDE)

def hexline(row, start, n=80):
    return " ".join(f"{b:02X}" for b in row[start:start + n])

def first_nonzero_col(row):
    nz = np.nonzero(row)[0]
    return int(nz[0]) if nz.size else -1

def trailing_zeros(row):
    nz = np.nonzero(row)[0]
    return RAW_STRIDE - 1 - int(nz[-1]) if nz.size else RAW_STRIDE

print("Row | mean | first_NZ | trailing_zeros | non-zero count")
for y in range(16):
    r = strip[y]
    print(f"{y:3d} | {r.mean():5.1f} | {first_nonzero_col(r):4d} | "
          f"{trailing_zeros(r):4d} | {np.count_nonzero(r):5d}/{RAW_STRIDE}")

print("\n=== Bytes 0..80 of clean and anomalous rows (head) ===")
for y in [3, 5, 7, 9, 11, 13]:
    print(f"R{y:2d} (mean={strip[y].mean():5.1f}): {hexline(strip[y], 0, 60)}")

print("\n=== Bytes 800..880 (mid) ===")
for y in [3, 5, 7, 9, 11, 13]:
    print(f"R{y:2d}: {hexline(strip[y], 800, 60)}")

print("\n=== Bytes 1840..1920 (tail) ===")
for y in [3, 5, 7, 9, 11, 13]:
    print(f"R{y:2d}: {hexline(strip[y], 1840, 80)}")

# Element-wise diff between row 5 and row 7 → where does divergence begin?
print("\n=== Where does row5 vs row7 divergence start? ===")
diff = np.abs(strip[5].astype(int) - strip[7].astype(int))
# Sliding window mean of |diff| over 32 bytes; the first window above
# threshold is where the rows really part company
thresh = 30
win = 32
ds = np.convolve(diff, np.ones(win) / win, mode="valid")
above = np.where(ds > thresh)[0]
print(f"first byte where |row5-row7| (windowed mean over {win} B) > {thresh}: "
      f"{above[0] if above.size else 'never'}")

print("\n=== Where does row10 vs row12 divergence start (EVEN pair)? ===")
diff = np.abs(strip[10].astype(int) - strip[12].astype(int))
ds = np.convolve(diff, np.ones(win) / win, mode="valid")
above = np.where(ds > thresh)[0]
print(f"first byte where |row10-row12| > {thresh}: "
      f"{above[0] if above.size else 'never'}")
