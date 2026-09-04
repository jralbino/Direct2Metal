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

**Golden — two sets in `hwbench.py`:** `GOLDEN` (detections as `(class, conf%)`: since
V189 **a real camera frame** captured with `make capture` — wall clock, **`[(74, 83)]`**;
V184's `bus.jpg` at 256² gave 3× person + bus `[(0,71),(0,77),(0,88),(5,85)]`) and
`GOLDEN_ABS` (a per-checkpoint numeric fingerprint): the `SERIAL_BOOT` build re-purposes
the existing `LOG_ABSMAX` checkpoints (L0, L1, L7, every C2f output, P3/P5 box+cls) to
print `[ABS] <tag> amax1e3=<max|x|×1000>`; `hwbench.py` diffs them with `--abs-tol`
(default ±1). Same binary in QEMU and on HW → they match to the thousandth. Regenerate
both sets after any model / exporter / test-image change (`make bench
BENCHFLAGS=--print-golden` prints paste-ready literals; or from QEMU):

> **V184 fix:** until V183 `test_image.bin` was a **320² fossil** (3×320×320 floats) fed to
> the 256² model — the kernel reads the first 3·N² floats, so the colour planes were
> misaligned and the graph saw striped garbage → `[DET] none` despite huge class logits.
> `tools/convert_image.py` now reads `YOLO_IN` from `bsp/bsp.h` (needs only Pillow +
> numpy; the `tools/env` venv has 0-byte interpreters and is unusable). Regenerating the
> tensor is what made detections appear — nothing in the graph changed.
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

**V191 on HW (2026-09-04):** per-position P8 (8-ch + 4-ch). **TOTAL 541 ms**
(`[P] cap=1 l0=13 bb=208 neck=109 head=189 render=18`), `detections: [(74,83)]`, fingerprint
16/16 bit-exact. Per-layer at 1 GHz: **head 3×3 105** (P3 box0/box1/cls0 24.5, cls1 32),
L4 33, L2 33 (bot 3×3 7.2 each), L12 24, L15 27, L21 19. `make HEAD_PROF=1` breaks down the
P3 head per conv; `make C2F_PROF=N` any C2f.

**V190:** conv1x1 input transpose (L1-set-thrash fix), TOTAL 664 ms; L2's cv2 1×1 55 → 7.8.

**V189 (previous):** stock clocks (`arm_freq=1000`, V187), TOTAL 799 ms — exactly ×1.6 the
600 MHz figure. Numbers before V187 in this file are at 600 MHz; don't mix them.

**V184 on HW (2026-09-04):** with the regenerated 256² `test_image` — `detections: PASS
[(0,71),(0,77),(0,88),(5,85)]`, `fingerprint: PASS (16/16)`, TOTAL 1284 ms (render 29→35 ms
for the four boxes). The stale-SD case was caught first: the run before the recopy
reproduced the V183 fossil fingerprint byte-for-byte → `FAIL 16/16` — exactly what the
fingerprint golden is for.

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

## Capturing a real camera frame as the golden input (V185)

`test_image.bin` doesn't have to be a JPEG: `make capture` turns a live sensor frame —
the exact `cam_frame` tensor the model consumes after debayer + ISP + AE — into one.

```
make capture                 # camera ON, dumps frame 30 (AE settled) over UART as base64
                             #  → cam_frame.bin (786 432 B, CRC-checked); ~3 min total
python3 tools/capture_to_test_image.py cam_frame.bin --preview cam_frame.png --no-install
                             # look at cam_frame.png; rerun `make capture` if the scene is wrong
python3 tools/capture_to_test_image.py cam_frame.bin      # installs app/<APP>/test_image.bin
make d2m_data.bin            # → recopy to SD (rm → cp → sync → umount)
make bench BENCHFLAGS=--print-golden                       # paste GOLDEN / GOLDEN_ABS
```

How it works: `kernel8_capture.img` = the serial-boot image built with `-DCAPTURE_FRAME=N`
(`CAPTURE ?= 30`) into separate `cap_*.o` objects, so the flag never leaks into the bench
image. `kernel_main` calls `camera_init()` instead of skipping it; on frame N
`run_yolo_complete` prints `[CAP] begin len= crc= n=` + base64 + `[CAP] end` (watchdog kicked
every 32 lines — the dump takes ~90 s at 115200) and then keeps running live inference, so
the `[DET]` lines after the dump tell you what the model sees in that scene right now.
`hwbench.py --capture PATH` reassembles and CRC32-checks the block and strips it from the log.
Point the camera at a scene with a recognisable COCO object first; no SD or extra wiring.
(Unicam's own `[CAP] TIMEOUT` message shares the prefix — the parser keys on `begin`/`end`.)

First run (2026-09-04): 786 432 B, CRC OK, live `[DET] c=0 %=67` on the same scene.
With `SHOW_CAMERA=1` (default since V186) the HDMI shows the live frame under the boxes
during a capture run, so you can frame the scene while it records; `render` costs ~22 ms.
Second run: person 92–96% on 24/29 frames.

**Exposure / white balance while capturing (V188).** The capture build prints
`[AE] cit= mean= sat= d=` on every AE update and `[AWB] r= b= unclipped=` every 32
frames. `sat` is the number of the 1024 AE grid samples at ≥ 250 raw — the number to
watch: the AE's highlight protection pushes exposure down until it is under 2 % (~20 of
1024), which from a cold start in a bright room takes ~20 updates ≈ 90 frames in this
build (hence `CAPTURE ?= 90`). If `sat` is still high at capture time, raise `CAPTURE=`.
"Blown whites" were exactly this (half the sensor saturated with the mean on target), not
white balance — see PLAN.md V188. A capture whose per-channel `>= 0.98` fraction
(`capture_to_test_image.py`) is under ~5 % for R/B and ~1 % for G is healthy; 40 % means
the AE hadn't settled.

## If `initramfs` doesn't load the blob

Symptom: `weights CRC mismatch` halt on frame 1. Try another address in both
`config.serial.txt` and `D2M_BLOB_ADDR` (`yolo_v8n.cpp`) — keep it below
`0x18000000` (384 MB, the ARM limit with `gpu_mem=128`) and clear of the kernel +
BSS (~`0x1A00000`). Verified working at `0x08000000` on the Zero 2 W firmware.
