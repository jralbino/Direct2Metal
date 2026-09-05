"""Simulate camara_direct's debayer pipeline against a Linux-captured DNG.

Goal: feed our debayer logic with bytes we KNOW are clean (libcamera-captured,
already debayered correctly via libcamera as ground truth) and see whether the
line-swap artefact appears. If it does, the bug is in our debayer/scale code
(reproducible in pure Python). If it doesn't, the bug is upstream — in our
Unicam/DMA capture path on the Pi.

Mirrors main.cpp::debayer_fullscreen byte-for-byte:
  - Bayer: RGGB at (0,0)  (sensor native, no libcamera flip)
  - Integer 2x2 block walk + 180° software flip
  - WB Q8 (R=416, G=256, B=448)
  - Output XRGB8888 packed as (R<<16)|(G<<8)|B  (PIL receives RGB)
"""
import sys, numpy as np, rawpy
from PIL import Image

SENSOR_W, SENSOR_H = 1536, 864
FB_W, FB_H = 1920, 1080
BLK_W, BLK_H = SENSOR_W // 2, SENSOR_H // 2  # 768, 432
WB_R, WB_G, WB_B = 416, 256, 448             # Q8 (256 == 1.0x)
BLACK = 64                                    # from DNG metadata


def load_raw_from_dng(path: str) -> np.ndarray:
    """Return uint16 array (864, 1536) of 10-bit values, oriented as our
    snap[] would be — i.e. RGGB at (0,0), scene rotated 180° vs. upright.
    libcamera's DNG is upright with BGGR (post HFLIP+VFLIP); rotating 180°
    inverts the flip and gives us the on-the-wire bare-metal layout."""
    raw = rawpy.imread(path).raw_image.copy().astype(np.uint16)
    assert raw.shape == (SENSOR_H, SENSOR_W), raw.shape
    return np.rot90(raw, 2).copy()  # 180° → matches our snap[]


def msb8(raw10: np.ndarray) -> np.ndarray:
    """High 8 bits of a 10-bit value, like raw10_msb8 in C."""
    return ((raw10 - BLACK).clip(0) >> 2).astype(np.uint8)
    # NOTE: subtracting black level here — main.cpp does NOT, but a fair
    # simulation should expose what each fix does. We'll run a second pass
    # without subtraction below to mimic the firmware exactly.


def debayer_fullscreen(raw10: np.ndarray, subtract_black: bool) -> np.ndarray:
    """Vectorised port of main.cpp::debayer_fullscreen."""
    # Apply (or not) black-level subtraction
    src = (raw10 - BLACK).clip(0) if subtract_black else raw10
    src8 = (src >> 2).astype(np.uint16)  # MSB8 in 16-bit headroom for WB mul

    # 180° flip block-level: blk_y = (BLK_H-1) - (fy * BLK_H) // FB_H
    fy = np.arange(FB_H)
    blk_y = (BLK_H - 1) - (fy * BLK_H) // FB_H        # (1080,)
    fx = np.arange(FB_W)
    blk_x = (BLK_W - 1) - (fx * BLK_W) // FB_W        # (1920,)

    sy_e = blk_y * 2
    sy_o = sy_e + 1
    sx_e = blk_x * 2
    sx_o = sx_e + 1

    # Gather per-row pairs, then per-col pairs (broadcast)
    R  = src8[sy_e[:, None], sx_e[None, :]]
    Gr = src8[sy_e[:, None], sx_o[None, :]]
    Gb = src8[sy_o[:, None], sx_e[None, :]]
    B  = src8[sy_o[:, None], sx_o[None, :]]
    G  = (Gr + Gb) >> 1

    R = ((R * WB_R) >> 8).clip(0, 255).astype(np.uint8)
    G = ((G * WB_G) >> 8).clip(0, 255).astype(np.uint8)
    B = ((B * WB_B) >> 8).clip(0, 255).astype(np.uint8)

    rgb = np.stack([R, G, B], axis=-1)
    return rgb


if __name__ == "__main__":
    dng = sys.argv[1] if len(sys.argv) > 1 else "test.dng"
    raw = load_raw_from_dng(dng)
    print(f"raw loaded: {raw.shape}, min={raw.min()}, max={raw.max()}, "
          f"mean={raw.mean():.1f}")

    # Mimic firmware exactly (no black subtraction)
    rgb_firmware = debayer_fullscreen(raw, subtract_black=False)
    Image.fromarray(rgb_firmware).save("/tmp/sim_firmware_exact.png")
    print(f"sim_firmware_exact.png saved  mean={rgb_firmware.mean(axis=(0,1))}")

    # Same pipeline + black-level subtraction
    rgb_blksub = debayer_fullscreen(raw, subtract_black=True)
    Image.fromarray(rgb_blksub).save("/tmp/sim_with_blacklevel.png")
    print(f"sim_with_blacklevel.png saved mean={rgb_blksub.mean(axis=(0,1))}")
