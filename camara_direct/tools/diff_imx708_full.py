"""Compare bare-metal k_imx708_full[] table against Linux full-mode i2c trace.

Linux source:    linux_extract/registers/imx708_writes_full_4608x2592.txt
Bare-metal:      src/imx708_regs.h  (parses k_imx708_full[] {reg,val} pairs)

The Linux trace is the ordered sequence libcamera writes during static init
+ first-frame AE convergence at 4608x2592. We want the bare-metal sequence
to be a strict superset of what's needed for a working stream.

Usage:
    python3 tools/diff_imx708_full.py
"""
import re
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LINUX_TRACE = ROOT / "linux_extract/registers/imx708_writes_full_4608x2592.txt"
BM_HEADER = ROOT / "src/imx708_regs.h"


def parse_linux(path):
    """Return list of (reg, val) preserving Linux write order, with last-write
    values collapsed for 16-bit re-writes mid-stream."""
    seq = []
    line_re = re.compile(r"^W\s+0x([0-9A-Fa-f]+)\s*=\s*0x([0-9A-Fa-f]+)")
    for raw in path.read_text().splitlines():
        m = line_re.match(raw)
        if not m:
            continue
        reg = int(m.group(1), 16)
        val = int(m.group(2), 16)
        if val > 0xFF:
            seq.append((reg, (val >> 8) & 0xFF))
            seq.append((reg + 1, val & 0xFF))
        else:
            seq.append((reg, val))
    return seq


def parse_bm(path):
    """Return list of (reg, val) from k_imx708_full[]."""
    txt = path.read_text()
    m = re.search(r"k_imx708_full\[\]\s*=\s*\{(.+?)\};", txt, re.DOTALL)
    if not m:
        sys.exit("ERR: k_imx708_full[] not found")
    body = m.group(1)
    seq = []
    for mm in re.finditer(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*\}", body):
        seq.append((int(mm.group(1), 16), int(mm.group(2), 16)))
    return seq


def last_write_dict(seq):
    d = OrderedDict()
    for reg, val in seq:
        d[reg] = val
    return d


def main():
    linux_seq = parse_linux(LINUX_TRACE)
    bm_seq = parse_bm(BM_HEADER)

    linux_d = last_write_dict(linux_seq)
    bm_d = last_write_dict(bm_seq)

    print(f"Linux writes (ordered, expanded): {len(linux_seq)}")
    print(f"Linux unique registers:           {len(linux_d)}")
    print(f"Bare-metal writes:                {len(bm_seq)}")
    print(f"Bare-metal unique registers:      {len(bm_d)}")
    print()

    linux_keys = set(linux_d)
    bm_keys = set(bm_d)

    missing = sorted(linux_keys - bm_keys)
    extra = sorted(bm_keys - linux_keys)
    common = sorted(linux_keys & bm_keys)
    differ = [r for r in common if linux_d[r] != bm_d[r]]

    print(f"== Last-value match: {len(common) - len(differ)} / {len(common)} common ==")
    if differ:
        print(f"\n!! {len(differ)} registers with DIFFERENT final values:")
        print(f"    {'REG':6}  {'BM':5}  {'Linux':5}  delta")
        for r in differ:
            print(f"    0x{r:04X}  0x{bm_d[r]:02X}   0x{linux_d[r]:02X}   bare-metal != Linux")

    if missing:
        print(f"\n-- {len(missing)} registers Linux writes that bare-metal MISSES:")
        for r in missing:
            print(f"    0x{r:04X} = 0x{linux_d[r]:02X}")

    if extra:
        print(f"\n++ {len(extra)} registers bare-metal writes that Linux DOES NOT:")
        for r in extra:
            print(f"    0x{r:04X} = 0x{bm_d[r]:02X}")

    # Synthesize a Linux ground-truth readback file in the format
    # compare_to_linux.py expects, so the runtime UART diff has a Linux side
    # for IMX708 (the original imx708_run.txt is empty because the capture
    # host couldn't open the i2c bus).
    out_path = ROOT / "linux_capture_linux/imx708_run_full_synth.txt"
    with open(out_path, "w") as f:
        f.write("# Synthetic Linux IMX708 final state for full 4608x2592 mode\n")
        f.write("# Generated from imx708_writes_full_4608x2592.txt last-write values\n")
        for r in sorted(linux_d):
            f.write(f"  reg=0x{r:04X} val=0x{linux_d[r]:02X}\n")
    print(f"Wrote synthetic Linux readback: {out_path}")

    print()
    print("== AE/AGC final values (live-stream convergence) ==")
    for reg, label in [(0x0202, "CIT_HI"), (0x0203, "CIT_LO"),
                       (0x0204, "AGAIN_HI"), (0x0205, "AGAIN_LO"),
                       (0x020E, "DGAIN_HI"), (0x020F, "DGAIN_LO"),
                       (0x0340, "FLL_HI"), (0x0341, "FLL_LO"),
                       (0x0342, "LLP_HI"), (0x0343, "LLP_LO"),
                       (0x0101, "ORIENT")]:
        l = linux_d.get(reg)
        b = bm_d.get(reg)
        ls = f"0x{l:02X}" if l is not None else "  --"
        bs = f"0x{b:02X}" if b is not None else "  --"
        ok = "ok" if l == b else "DIFF"
        print(f"  0x{reg:04X} {label:10}  BM={bs}  Linux={ls}  {ok}")


if __name__ == "__main__":
    main()
