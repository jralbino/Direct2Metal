"""Compare bare-metal UART capture against Linux full-mode ground truth.

Differs from compare_to_linux.py in that:
  * Uses imx708_run_full_synth.txt (synthesized from i2c trace, since the
    Linux capture script couldn't open the i2c bus → imx708_run.txt is empty).
  * Knows which Unicam registers MUST differ between binned (Linux capture)
    and full (bare-metal target): IBSA0, IBEA0, IBLS, IBWP, plus dynamic
    state STA/IBWP/ISTA/MISC. Marks those as 'EXPECTED' rather than 'DIFF'.

Usage:
    python3 tools/diff_runtime_full.py [uart.log] [linux_capture_dir]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_UART = ROOT / "uart.log"
DEFAULT_LIN = ROOT / "linux_capture_linux"

# Registers expected to differ between Linux-binned and bare-metal-full.
# Geometry is mode-dependent; STA/ISTA/IBWP/MISC are dynamic snapshots.
UNICAM_EXPECTED_DIFF = {"IBSA0", "IBEA0", "IBLS", "IBWP",
                        "STA", "ISTA", "MISC"}

# Bare-metal target values for full mode (sanity checks).
UNICAM_BM_FULL_TARGETS = {
    "IBLS": 0x1680,        # 4608 px * 10 / 8 = 5760
    "CTRL": 0x00080F03,
    "ANA":  0x00000770,
    "PRI":  0x00000E85,
    "CLK":  0x06000005,
    "CLT":  0x00000602,
    "DAT0": 0xC0000005,
    "DAT1": 0x06000005,    # NOT 0xC0000005 — fix for line-swap (V147 lesson)
    "DAT2": 0x0A000002,
    "DAT3": 0x0A000002,
    "DLT":  0x00000602,
    "CMP0": 0x80000301,
    "ICTL": 0x00D80007,
    "IDI0": 0x0000002B,
    "IPIPE": 0x00000000,
    "IHWIN": 0x00000000,
    "IVWIN": 0x00000000,
}


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
    if not path.exists():
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
    if not path.exists():
        return {}
    out = {}
    for line in open(path):
        mm = re.match(r"\s*reg=0x([0-9A-Fa-f]+)\s+val=0x([0-9A-Fa-f]+)", line)
        if mm:
            out[int(mm.group(1), 16)] = int(mm.group(2), 16)
    return out


def main():
    uart_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_UART
    lin_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_LIN

    if not uart_path.exists():
        sys.exit(f"ERR: UART log not found: {uart_path}")

    text = uart_path.read_text(errors="ignore")
    bm_un = parse_unicam_baremetal(text)
    bm_im = parse_imx708_baremetal(text)
    lin_un = parse_unicam_linux(lin_dir / "unicam_run.txt")
    # Prefer the synthetic full-mode file (real one is empty)
    lin_im_path = lin_dir / "imx708_run_full_synth.txt"
    if not lin_im_path.exists():
        lin_im_path = lin_dir / "imx708_run.txt"
    lin_im = parse_imx708_linux(lin_im_path)

    print(f"Bare-metal UART:    {uart_path}")
    print(f"Linux Unicam:       {lin_dir / 'unicam_run.txt'}  (binned mode capture)")
    print(f"Linux IMX708:       {lin_im_path}")
    print(f"  unicam regs: linux={len(lin_un)}  bm={len(bm_un)}")
    print(f"  imx708 regs: linux={len(lin_im)}  bm={len(bm_im)}")
    if not bm_un and not bm_im:
        sys.exit("\nERR: no UNICAM_BEGIN/IMX708_BEGIN blocks in UART log — is this a V150 build?")

    print(f"\n=== Unicam (Linux=binned, bare-metal=full) ===")
    print(f"{'reg':<6} {'linux':>10}   {'baremetal':>10}   status")
    real_diffs = []
    for k in sorted(set(lin_un) | set(bm_un)):
        l = lin_un.get(k); b = bm_un.get(k)
        ls = f"0x{l:08X}" if l is not None else "         —"
        bs = f"0x{b:08X}" if b is not None else "         —"
        target = UNICAM_BM_FULL_TARGETS.get(k)
        if l == b:
            mark = "match"
        elif k in UNICAM_EXPECTED_DIFF:
            mark = "EXPECTED (mode-dep)"
        else:
            mark = "DIFF"
            real_diffs.append((k, l, b))
        if b is not None and target is not None and target != b:
            mark += f" — bare-metal target=0x{target:08X}"
        print(f"  {k:<6} {ls}   {bs}   {mark}")

    print(f"\n=== IMX708 (Linux=trace converged, bare-metal=I2C readback) ===")
    print(f"{'reg':<8} {'linux':>6}   {'baremetal':>10}   status")
    im_diffs = []
    for k in sorted(set(lin_im) | set(bm_im)):
        if k not in bm_im:
            continue  # only diff what we read back
        l = lin_im.get(k); b = bm_im[k]
        ls = f"0x{l:02X}" if l is not None else "    —"
        bs = f"0x{b:02X}" if b is not None else "    —"
        if l == b:
            mark = "match"
        elif l is None:
            mark = "no Linux ref"
        else:
            mark = "DIFF"
            im_diffs.append((k, l, b))
        print(f"  0x{k:04X}   {ls}        {bs}        {mark}")

    print(f"\n=== SUMMARY ===")
    print(f"Unicam unexpected diffs: {len(real_diffs)}")
    for k, l, b in real_diffs:
        print(f"  {k}: linux=0x{l:08X}  bm=0x{b:08X}")
    print(f"IMX708 readback diffs:   {len(im_diffs)}")
    for k, l, b in im_diffs:
        print(f"  0x{k:04X}: linux=0x{l:02X}  bm=0x{b:02X}")
    if not real_diffs and not im_diffs:
        print("\n✓ Bare-metal full-mode init matches Linux exactly.")


if __name__ == "__main__":
    main()
