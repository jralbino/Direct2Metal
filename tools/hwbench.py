#!/usr/bin/env python3
"""
hwbench.py — unattended flash + capture + parse for Direct2Metal on real HW.

Loop per iteration (V183):
  1. pulse the adapter's RTS line -> Pi RUN pad = warm reset
  2. speak the raspbootin protocol over the same UART, upload the code-only
     kernel (FP32 weights + test_image come from the SD via `initramfs`)
  3. capture the UART log until the frame count / timeout is reached
  4. parse [T] total, the [P] stage buckets, the PER-LAYER PROFILE, the
     [DET] c=<cls> %=<conf> lines and the [ABS] <tag> amax1e3=<n> fingerprints
  5. diff detections + fingerprints against the golden sets -> PASS / FAIL

Typical use:
    make bench                      # = make serial-kernel + this, 3 frames
    make bench FRAMES=5 PORT=/dev/ttyUSB1
    make bench BENCHFLAGS="--raw"   # also dump the raw UART text

One-time SD prep (see tools/HWBENCH.md):  make sdcard  -> copy sdcard/* to the SD.

Wiring: CH340 TXD->Pi GPIO15(RXD pin10), CH340 RXD->Pi GPIO14(TXD pin8),
GND<->GND, and CH340 RTS -> Pi RUN test point (1k series resistor recommended).
Default polarity is right for a bare CH340 pad — do NOT pass --reset-invert
unless your adapter buffers/inverts RTS (it would hold RUN low all session).
Never power-cycle to reset: the CH340 re-enumerates on the ground transient.
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not found:  pip install pyserial")

# ── Golden sets for app/yolo_v8n_coco/test_image.bin, FP32 path ──────────────
# Generated from the SERIAL_BOOT build in QEMU (deterministic, single-core, no
# camera) and confirmed on HW:
#   make kernel8_serial.img d2m_data.bin
#   qemu-system-aarch64 -M raspi3b -kernel kernel8_serial.img \
#     -device loader,file=d2m_data.bin,addr=0x08000000 -serial stdio -display none
# Regenerate both whenever the model / exporter / test image changes.
#
# Detections as (class, conf%) — the [DET] line carries no position. bus.jpg at
# 256² (V184 — the V169..V183 tensor was a 320² fossil that misaligned the colour
# planes and gave `[DET] none`): 3× person + 1× bus, same objects D2M's YOLOv5n
# found at 320².
GOLDEN = [(0, 71), (0, 77), (0, 88), (5, 85)]
# Per-checkpoint max|activation| x 1000 (int), from the [ABS] lines. Compared with
# --abs-tol (default 1 = one thousandth) to absorb the x1000 rounding.
# V184, QEMU raspi3b, kernel8_serial.img + d2m_data.bin (weights.bin CRC 0x176A3D5A,
# WEIGHTS_SIZE 12608056; test_image.bin 786432 B = bus.jpg → [3][256][256] via
# tools/convert_image.py). Identical on frames 1/2.
GOLDEN_ABS = {
    "L0": 44475, "L1": 104883, "L2": 11016, "L4": 10433, "L6": 8294, "L7": 6740,
    "L8": 4555, "SPPF": 3929, "L12": 4929, "L15": 5007, "L18": 6814, "L21": 5779,
    "P5_BOX": 11541, "P5_CLS": 29104, "P3_BOX": 15950, "P3_CLS": 25018,
}

# HW line is "[DET] id=<track> c=<cls> %=<conf>[ miss=<n>]" (tracker output);
# match c=/%= anywhere after the tag so id=/miss= are tolerated.
DET_RE    = re.compile(r"\[DET\][^\n]*?\bc=(\d+)\s+%=(\d+)")
ABS_RE    = re.compile(r"\[ABS\]\s+(\S+)\s+amax1e3=(\d+)")
TOTAL_RE  = re.compile(r"\[T\]\s+(\d+)\s*ms")
PLINE_RE  = re.compile(r"\[P\]\s+(.*)$", re.M)
KV_RE     = re.compile(r"([a-z0-9_]+)=(\d+)")
# "  L6  C2f ...: 68 us" (2 spaces) and the nested "    C2f.bot cv1 3x3: ..." (4 spaces);
# indentation beyond the first 2 spaces is kept in the name so the report shows the tree.
PROF_RE   = re.compile(r"^\s{2}(\s*\S.*?):\s*(\d+)\s*us\s*$")
FRAME_RE  = re.compile(r"(?=^\[F\d+s?\]\s)", re.M)


def reset_pi(ser, hold, invert, settle):
    """RTS asserted (True) drives the bare CH340 pad LOW -> RUN to GND -> reset."""
    lo, hi = (False, True) if invert else (True, False)
    ser.rts = hi
    time.sleep(0.05)
    ser.rts = lo            # into reset
    time.sleep(hold)
    ser.rts = hi            # release -> boot
    ser.reset_input_buffer()
    time.sleep(settle)


def wait_for_breq(ser, timeout):
    """raspbootin sends 0x03 0x03 0x03 once uart_init() is up."""
    deadline = time.time() + timeout
    run = 0
    seen = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        seen += b
        run = run + 1 if b == b"\x03" else 0
        if run >= 3:
            return True
    sys.stderr.write("no boot request; last bytes: %r\n" % bytes(seen[-64:]))
    return False


def upload(ser, blob, ack_timeout):
    ser.write(len(blob).to_bytes(4, "little"))
    ser.flush()
    deadline = time.time() + ack_timeout
    resp = bytearray()
    while time.time() < deadline and len(resp) < 2:
        resp += ser.read(1)
    if bytes(resp) == b"SE":
        raise RuntimeError("loader rejected size (SE)")
    if bytes(resp) != b"OK":
        raise RuntimeError("no OK from loader, got %r" % bytes(resp))
    ser.write(blob)
    ser.flush()


def capture(ser, frames, timeout, logfile):
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if buf.count(b"[T] ") >= frames:
                time.sleep(0.5)          # let the PER-LAYER PROFILE drain
                buf += ser.read(ser.in_waiting or 0)
                break
    text = buf.decode("utf-8", "replace")
    if logfile:
        with open(logfile, "w") as f:
            f.write(text)
    return text


def parse_frame(block):
    dets = [(int(c), int(p)) for c, p in DET_RE.findall(block)]
    absmax = {tag: int(v) for tag, v in ABS_RE.findall(block)}
    total = TOTAL_RE.search(block)
    pline = PLINE_RE.search(block)
    buckets = {k: int(v) for k, v in KV_RE.findall(pline.group(1))} if pline else {}
    prof = [(m.group(1), int(m.group(2))) for m in
            (PROF_RE.match(l) for l in block.splitlines()) if m]
    return {
        "dets": dets,
        "absmax": absmax,
        "total_ms": int(total.group(1)) if total else None,
        "buckets": buckets,
        "profile": prof,
    }


def split_frames(text):
    parts = FRAME_RE.split(text)
    return [p for p in parts if "[T] " in p]


def diff_absmax(got, exp, tol):
    """Return list of (tag, expected, got) mismatches beyond tol; missing tags count."""
    bad = []
    for tag, e in exp.items():
        g = got.get(tag)
        if g is None or abs(g - e) > tol:
            bad.append((tag, e, g))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--kernel", default="kernel8_serial.img", help="code-only image to upload")
    ap.add_argument("--frames", type=int, default=3, help="frames to capture before returning")
    ap.add_argument("--timeout", type=float, default=150.0, help="capture timeout (s)")
    ap.add_argument("--boot-timeout", type=float, default=30.0, help="wait for loader boot request (s)")
    ap.add_argument("--reset-hold", type=float, default=0.25, help="RUN-low duration (s)")
    ap.add_argument("--reset-settle", type=float, default=0.05, help="pause after release (s)")
    ap.add_argument("--reset-invert", action="store_true", help="flip RTS polarity (normally NOT needed)")
    ap.add_argument("--no-reset", action="store_true", help="skip RTS reset (power-cycle yourself — discouraged)")
    ap.add_argument("--log", default="hwbench.log")
    ap.add_argument("--raw", action="store_true", help="dump the raw captured UART text too")
    ap.add_argument("--no-golden", action="store_true", help="skip both golden diffs (exit 0 on capture)")
    ap.add_argument("--abs-tol", type=int, default=1, help="tolerance on amax1e3 fingerprints (thousandths)")
    ap.add_argument("--print-golden", action="store_true",
                    help="print the last frame's dets/absmax as python literals (to paste into GOLDEN*)")
    args = ap.parse_args()

    with open(args.kernel, "rb") as f:
        blob = f.read()
    print(f"kernel  {args.kernel}  {len(blob)} bytes  (~{len(blob)*10/args.baud:.0f}s upload @ {args.baud})")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    # pyserial asserts RTS on open -> that would hold RUN low. Release it first
    # (released level = "hi" in reset_pi's polarity convention).
    ser.rts = args.reset_invert          # False normally, True if inverted
    time.sleep(0.05)
    try:
        if not args.no_reset:
            print("reset   RTS pulse")
            reset_pi(ser, args.reset_hold, args.reset_invert, args.reset_settle)
        else:
            print("reset   skipped — power-cycle the Pi now")
            ser.reset_input_buffer()

        if not wait_for_breq(ser, args.boot_timeout):
            sys.exit("FAIL: no raspbootin boot request (check wiring / SD kernel8.img / baud)")
        print("upload  loader ready, sending kernel...")
        t0 = time.time()
        upload(ser, blob, ack_timeout=5.0)
        print(f"upload  done in {time.time()-t0:.1f}s, capturing {args.frames} frame(s)...")

        text = capture(ser, args.frames, args.timeout, args.log)
    finally:
        ser.close()

    if args.raw:
        print("\n----- raw -----\n" + text + "\n---------------\n")

    frames = split_frames(text)
    if not frames:
        sys.exit("FAIL: no complete frame captured — see " + args.log)

    # report the last frame (steady state; frame 1 includes the one-time CRC + cold caches)
    fr = parse_frame(frames[-1])
    print(f"\n=== frame {len(frames)} (of {len(frames)} captured) ===")
    print(f"TOTAL: {fr['total_ms']} ms")
    for k, v in fr["buckets"].items():
        print(f"  {k:<10} {v:>5} ms")
    if fr["profile"]:
        print("  per-layer:")
        for name, us in fr["profile"]:
            print(f"    {name:<34} {us/1000:8.2f} ms")

    got = sorted(fr["dets"])
    print(f"\ndetections: {got if got else 'none'}")
    print(f"fingerprint: {len(fr['absmax'])} [ABS] checkpoints")

    if args.print_golden:
        print("\nGOLDEN =", got)
        print("GOLDEN_ABS =", fr["absmax"])

    if args.no_golden:
        sys.exit(0)

    ok = True
    if GOLDEN or got:
        exp = sorted(GOLDEN)
        if got != exp:
            ok = False
            print(f"detections: FAIL  expected {exp}  got {got}")
        else:
            print("detections: PASS")
    if GOLDEN_ABS:
        bad = diff_absmax(fr["absmax"], GOLDEN_ABS, args.abs_tol)
        if bad:
            ok = False
            print(f"fingerprint: FAIL  ({len(bad)} of {len(GOLDEN_ABS)} checkpoints off by > {args.abs_tol})")
            for tag, e, g in bad:
                print(f"  {tag:<10} expected {e:>9}  got {g if g is not None else 'missing'}")
        else:
            print(f"fingerprint: PASS  ({len(GOLDEN_ABS)} checkpoints within ±{args.abs_tol})")
    elif not GOLDEN:
        print("(no golden sets defined — run with --print-golden and paste into hwbench.py)")
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
