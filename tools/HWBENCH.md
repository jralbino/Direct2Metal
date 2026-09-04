# Unattended hardware bench (`make bench`) — V183

Edit → `make` → flash → capture UART → parse timings → diff detections, with **no
SD swaps and no hands on the board**. The code kernel travels over the UART you
already have; the FP32 weights stay on the SD; a wire from the adapter's RTS to
the Pi's RUN pad does the reset.

```
   ┌─────────── dev machine ───────────┐         ┌──── RPi Zero 2 W ────┐
   │  make serial-kernel                │         │                      │
   │  tools/hwbench.py  ───────────────────USB────┤ CH340 USB-TTL        │
   │    1. pulse RTS  ────────────────────────────┤   RTS ─1kΩ─► RUN pad │  warm reset
   │    2. raspbootin proto, send 60 KB ──TXD────►│   TXD ──────► GPIO15 │  (pin 10, RXD)
   │    3. capture ◄──────────────────────RXD─────┤   RXD ◄────── GPIO14 │  (pin 8, TXD)
   │    4. parse + PASS/FAIL            │         │   GND ◄─────► GND    │  (pin 6/9)
   └───────────────────────────────────┘         │  micro-USB PWR: wall │  (always on)
                                                 │  SD: firmware + stub │
                                                 │      + d2m_data.bin  │
                                                 └──────────────────────┘
```

## Wiring

| CH340 pad | Pi Zero 2 W | Notes |
|---|---|---|
| TXD | GPIO15 / pin 10 (RXD) | data PC→Pi |
| RXD | GPIO14 / pin 8 (TXD)  | data Pi→PC |
| GND | pin 6 (or 9, 14, 20…) | common ground — **required** |
| RTS | RUN test point        | 1 kΩ in series; RUN pulled to GND = reset |

- CH340 jumper on **3V3**. Do **not** connect its 5V/3V3 pin; the Pi has its own supply.
- Default RTS polarity is correct for a bare CH340 pad (RTS deasserted = HIGH = RUN
  released; the script pulses it LOW). **Do not pass `--reset-invert`** — it holds
  RUN low for the whole session. Tune the pulse with `--reset-hold` (0.25 s works).
- **Never reset by power-cycling.** The CH340 re-enumerates on the shared-ground
  transient and the capture dies. Reset is RTS-only.
- Between runs the port is closed and the ch341 driver may leave RTS asserted → the
  Pi sits in reset until `make bench` opens the port. Harmless.
- The device node needs to be writable: a udev rule
  `SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666"`
  in `/etc/udev/rules.d/99-ch340.rules` (or add yourself to `dialout`).

## One-time SD prep

```
make sdcard         # builds the stub + d2m_data.bin, stages ./sdcard/
```
Copy onto the SD boot partition, alongside the RPi firmware you already have
(`bootcode.bin`, `start_x.elf`, `fixup_x.dat`):

| SD file | comes from |
|---|---|
| `kernel8.img` | `sdcard/kernel8.img` (= `tools/raspbootin64/raspbootin64.img`, 936 B) |
| `config.txt`  | `sdcard/config.txt` (= `build/config.serial.txt`) |
| `d2m_data.bin`| `sdcard/d2m_data.bin` — **rebuild + recopy only when the model changes** |

`config.serial.txt` = `build/config.txt` + `initramfs d2m_data.bin 0x08000000`; the
VPU loads the blob before the kernel starts. The `-DSERIAL_BOOT` build of
`app/yolo_v8n_coco/yolo_v8n.cpp` reads `weights_start` from `0x08000000` and
`test_image` from `0x08000000 + align16(WEIGHTS_SIZE)` — the same offsets
`tools/pack_data.py` produces. It also skips `camera_init()` so inference always
runs the golden `test_image`, and refuses to build with `USE_INT8*` (fp32 only).

## Iteration loop

```
make bench                       # = make serial-kernel + hwbench.py, 3 frames
make bench FRAMES=5 PORT=/dev/ttyUSB1
make bench BENCHFLAGS="--raw"    # also dump the full UART text
```

`hwbench.py` reports the **last** captured frame (frame 1 carries the one-time
CRC + cold caches): `[T]` total, the `[P]` stage buckets (`cap l0 bb neck head
render`), the full **per-layer profile** (`prof_mark`s in `run_yolo_complete` +
a C2f breakdown of L6), and the detections as `(class, conf%)` pairs diffed
against `GOLDEN` in `tools/hwbench.py`. Exit code 2 on a mismatch — usable in a
script. `--no-golden` skips the diff.

**Detections come from the tracker** (`[DET] id=N c=<cls> %=<conf>[ miss=n]`), so
they converge over the first couple of frames on a static image — always compare
the last frame, never frame 1.

**Golden — two sets in `hwbench.py`:** `GOLDEN` (detections as `(class, conf%)`) and
`GOLDEN_ABS` (a per-checkpoint numeric fingerprint). `test_image.bin` currently yields
**no detections** above `CONF_THRESH` on the FP32 path, so `GOLDEN` is empty and the
fingerprint is the real check: the `SERIAL_BOOT` build re-purposes the existing
`LOG_ABSMAX` checkpoints (L0, L1, L7, every C2f output, P3/P5 box+cls) to print
`[ABS] <tag> amax1e3=<max|x|×1000>`; `hwbench.py` diffs them with `--abs-tol` (default
±1). Same binary in QEMU and on HW → they match to the thousandth. Regenerate both sets
after any model / exporter / test-image change (`make bench BENCHFLAGS=--print-golden`
prints paste-ready literals; or from QEMU):
```
make kernel8_serial.img d2m_data.bin
qemu-system-aarch64 -M raspi3b -kernel kernel8_serial.img \
  -device loader,file=d2m_data.bin,addr=0x08000000 -serial stdio -display none
```
(`-device loader` puts the blob where the VPU's `initramfs` would.) In QEMU the
`[SIM]` camera oracle is NOT active — `SERIAL_BOOT` is a plain HW build, so this
is also the cleanest way to run the fp32 graph on `test_image` without the camera.

**Absolute timing note:** `build/config.serial.txt` runs the low-power clock profile
(`arm_freq=600`, `core_freq=250`, `sdram_freq=400`), same as `build/config.txt`.
Good for A/B of perf changes; not comparable to numbers taken at other clocks.

**First verified run (V183, 2026-09-03):** upload 80 KB in 7 s, 4 frames, `fingerprint:
PASS (16 checkpoints within ±1)`, **TOTAL 1276 ms** (`[P] cap=1 l0=21 bb=502 neck=289
head=431 render=29`). A/B against the same image built with `-DD2M_NO_P8`: **1359 → 1276
ms (−6.1%)** — P3 head 320→260, L4 96→82, L15 119→112, L2 unchanged (its bottleneck is
16-ch → 4-ch kernel, untouched by P8). Per-layer hot spots: P3 head 260, L2 C2f @S4 154,
P4 head 121, L15 112, L4 82. Heads = 34% of the frame.

**A/B recipe:** build a second serial image with the kill-switch and bench it —
```
docker run … rpi-forge sh -c 'make ser_multicore.o CXXFLAGS="$(CXXFLAGS) -DD2M_NO_P8" …'
```
(or simply add `-DD2M_NO_P8` to `SERFLAGS` in the Makefile, `make bench`, revert).

## Two gotchas that were fixed (don't reintroduce)

1. **UART clock.** `tools/raspbootin64/uart.c` pins the UART clock to **48 MHz**
   (IBRD=26, FBRD=3) to match `bsp/kernel.cpp:uart_init()`. The upstream stub set
   4 MHz, so after the jump the divisor was wrong and the UART emitted a solid
   stream of `0x00`. If you revendor the stub, re-apply.
2. **Camera off.** Without the `SERIAL_BOOT` skip, `camera_init()` succeeds on HW and
   inference runs on live frames — non-deterministic, and the golden diff fails
   for no real reason.

## If `initramfs` doesn't load the blob

Symptom: `weights CRC mismatch` halt on frame 1. Try another address in both
`config.serial.txt` and `D2M_BLOB_ADDR` (`yolo_v8n.cpp`) — keep it below
`0x18000000` (384 MB, the ARM limit with `gpu_mem=128`) and clear of the kernel +
BSS (~`0x1A00000`). Verified working at `0x08000000` on the Zero 2 W firmware.
