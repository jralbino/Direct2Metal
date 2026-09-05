"""Diff bare-metal capture report against Linux/libcamera ground truth.

Inputs:
  uart.log                            — bare-metal UART capture
  linux_capture/unicam_run.txt        — Linux Unicam reg dump (live stream)
  linux_capture/imx708_run.txt        — Linux IMX708 i2c reg dump (live stream)
  linux_capture/frame.dng (optional)  — Linux RAW frame for byte comparison

Outputs (stdout):
  Side-by-side diff of every Unicam register that appears on both sides,
  with "==" / "!=" marker and Linux/baremetal hex values.
  Same for every IMX708 register.
  Highlights only the divergent rows and a bottom-line summary count.

The intent is one-glance triage: each "!=" row is a concrete difference
between our init sequence and what libcamera leaves running, and is the
list of fixes to make.

Usage:
  python3 tools/compare_to_linux.py uart.log linux_capture/
"""
import re
import sys
from pathlib import Path


def parse_unicam_baremetal(text):
    m = re.search(r"UNICAM_BEGIN(.*?)UNICAM_END", text, re.DOTALL)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if mm:
            out[mm.group(1).upper()] = int(mm.group(2), 16)
    return out


def parse_unicam_linux(path):
    if not Path(path).exists():
        return {}
    out = {}
    for line in open(path):
        mm = re.match(r"\s*(\w+)\s+offset=\S+\s+val=0x([0-9A-Fa-f]+)", line)
        if mm:
            out[mm.group(1).upper()] = int(mm.group(2), 16)
    return out


def parse_imx708_baremetal(text):
    m = re.search(r"IMX708_BEGIN(.*?)IMX708_END", text, re.DOTALL)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*reg=0x([0-9A-Fa-f]+)\s+val=0x([0-9A-Fa-f]+)", line)
        if mm:
            out[int(mm.group(1), 16)] = int(mm.group(2), 16)
    return out


def parse_imx708_linux(path):
    if not Path(path).exists():
        return {}
    out = {}
    for line in open(path):
        mm = re.match(r"\s*reg=0x([0-9A-Fa-f]+)\s+val=0x([0-9A-Fa-f]+)", line)
        if mm:
            out[int(mm.group(1), 16)] = int(mm.group(2), 16)
    return out


def diff_table(name, lin, bm, *, hex_width=8):
    keys = sorted(set(lin) | set(bm))
    diffs = []
    print(f"\n=== {name} ({len(keys)} regs) ===")
    print(f"{'reg':<10} {'linux':>{hex_width+2}}   {'baremetal':>{hex_width+2}}   match")
    for k in keys:
        l = lin.get(k)
        b = bm.get(k)
        ls = "—".rjust(hex_width + 2) if l is None else f"0x{l:0{hex_width}X}"
        bs = "—".rjust(hex_width + 2) if b is None else f"0x{b:0{hex_width}X}"
        if l is None or b is None:
            mark = "??"
        elif l == b:
            mark = "=="
        else:
            mark = "!="
            diffs.append((k, l, b))
        kk = f"0x{k:04X}" if isinstance(k, int) else str(k)
        print(f"{kk:<10} {ls:>{hex_width+2}}   {bs:>{hex_width+2}}   {mark}")
    return diffs


def main():
    uart = sys.argv[1] if len(sys.argv) > 1 else "uart.log"
    lin_dir = Path(sys.argv[2] if len(sys.argv) > 2 else "linux_capture")

    text = open(uart, errors="ignore").read()
    bm_un = parse_unicam_baremetal(text)
    bm_im = parse_imx708_baremetal(text)
    lin_un = parse_unicam_linux(lin_dir / "unicam_run.txt")
    lin_im = parse_imx708_linux(lin_dir / "imx708_run.txt")

    print(f"Bare-metal UART:    {uart}")
    print(f"Linux baseline dir: {lin_dir}")
    print(f"  unicam regs: linux={len(lin_un)}  bm={len(bm_un)}")
    print(f"  imx708 regs: linux={len(lin_im)}  bm={len(bm_im)}")

    un_diffs = diff_table("Unicam", lin_un, bm_un, hex_width=8)
    im_diffs = diff_table("IMX708", lin_im, bm_im, hex_width=2)

    print(f"\n=== SUMMARY ===")
    print(f"Unicam: {len(un_diffs)} divergent registers")
    for k, l, b in un_diffs:
        print(f"  {k}: linux=0x{l:08X}  baremetal=0x{b:08X}  → set baremetal := linux value")
    print(f"IMX708: {len(im_diffs)} divergent registers")
    for k, l, b in im_diffs:
        print(f"  0x{k:04X}: linux=0x{l:02X}  baremetal=0x{b:02X}  → set baremetal := linux value")

    if not un_diffs and not im_diffs and lin_un and lin_im:
        print("\n✓ All compared registers match Linux exactly.")


if __name__ == "__main__":
    main()
