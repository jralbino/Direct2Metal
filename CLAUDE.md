# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**Direct2Metal** — YOLOv8n object detection (320×192, 80 COCO classes, anchor-free DFL)
running bare-metal on a Raspberry Pi Zero 2 W (BCM2837, 4× Cortex-A53). No OS: custom boot,
MMU, D-cache, 4-core dispatch, NEON kernels, and a working bare-metal MIPI CSI-2 driver for
the Pi Camera Module 3 (IMX708). Real-hardware results: **264 ms/frame sustained** at `YOLO_W=320,
YOLO_H=192` (V199 — all four cores compute, display pumped between convs; V197 367 ms with explicit input
padding so the P8 fast path covers 100 % of positions; V195 435 ms
with P8 for stride 2 + noinline accumulator leaves; V192 504 ms at full sensor
FoV, V191 541 ms at 256², V189 799 ms, V183 1276 ms at 600 MHz). Every timing claim comes with
its `[SOC] arm= temp= thr=` line (V194). **Validate perf with a long run, never the 3-frame
bench**: until V196 the firmware dropped the ARM clock 1000 → 600 MHz ~60 s after boot
(435 → 705 ms) and a 3-frame bench could not see it; `soc_clock_boost()` now asks for the
clock over the mailbox at boot (301 frames flat at 435 ms, 66 °C). The graph is fully convolutional so the same YOLOv8n weights run
at any input divisible by 32; `bsp.h` holds `YOLO_W`/`YOLO_H`. The four P3-head conv3×3
(~90 ms of the 98 ms P3 head) are still the biggest item. With `FRAME_SKIP_N=4` (default) the camera + HUD refresh at
the capture rate and boxes update every 4th frame.
This tree is the live one; `~/projects/D2M` is a stale clone of the same GitHub repo (V76)
— do not develop there.

**The engineering logs are `GOALS.md` (roadmap + verdicts), `PLAN.md` (version log,
V1→V165) and `camera_debug.md` (Unicam bring-up).** `README.md` is a stale YOLOv5n copy — trust GOALS/PLAN over it. Record new findings in
GOALS.md / PLAN.md version-stamped; the session reached V200 (705 → 264 ms sustained across V195-V199; V200 left a single inference path).

**The working tree is clean as of V195.** The ~110 pending files were committed then: 50 were
a stray `chmod +x` sweep (restored to 100644), 14 were build artifacts tracked *and*
gitignored (`git rm --cached`), and the only real sources — `bsp/multicore.h` +
`bsp/camera_unicam.cpp` — turned out to be the missing half of already-committed code, so
`HEAD` did not compile (`yolo_v8n.cpp:586: 'parallel_async_done' was not declared`). Still
never `git stash` / `git checkout -- .` on a dirty tree without looking at it first.

## Build & run

Toolchain is AArch64 cross-GCC inside the Docker image built from `Dockerfile` (`rpi-forge`).
Always pass `--user` so artifacts are not root-owned:

```bash
docker build -t rpi-forge .
docker run --rm --user $(id -u):$(id -g) -v $(pwd):/app rpi-forge make kernel8.img
```

| Command | Purpose |
|---|---|
| `make kernel8.img` | Hardware build (`APP=yolo_v8n_coco` default). |
| `make sim` | `-DSIMULATION` build **and** QEMU run. Self-terminates after 3 frames via semihosting (`hlt #0xf000`). Camera is *simulated present* → runs the synthetic-frame path, `[DET] none`. Use it for camera-driver sequencing, not for detection correctness. |
| `make sim_elf` | Same build, no QEMU. |
| `make USE_INT8=1` / `USE_INT8_W8A8=1` | Tier 1 / Tier 2 INT8 builds. **Closed** (see below) but kept building. |
| `make FRAME_SKIP_N=1` | B4 frame-skip. Default is **4** since V187 (camera + HUD at capture rate, inference on 1 in 4 frames); N=1 infers every frame. The bench and capture images always force N=1 via `SERFLAGS` (`hwbench.py` needs `[P]`/`[ABS]` on the frame it parses). |
| `make AWB=0` | Freeze the ISP white balance at the tuning-file daylight gains (WB_R=535, WB_B=455 Q8). Default **1** (V188): grey-world AWB once per frame in `debayer_awb_update()`, prints `[AWB] r= b=` every 32 frames. |
| `make DEBUG=1` | Camera thumbnail in the canvas. |
| `make SHOW_CAMERA=0` | Restore the V167 dark canvas. Default **1** (V186) paints the live 640×360 frame under the bboxes via `camera_render_fullres()` — measured +21 ms/frame in `render` (single-core; the V164 multi-core debayer was removed in V167). |
| `make EXTRA_DEF=-D…` | Ad-hoc A/B defines without editing the Makefile: `-DD2M_NO_S2P8` (P8 on stride 1 only, V195), `-DD2M_NO_P8` (no conv2d fast path). |
| `make clean` | Removes `*.o *.elf *.img` (all gitignored — safe). |

`make` flags → `-D` defines: `USE_INT8_WEIGHTS`, `USE_INT8_W8A8`, `W8A8_DEBUG`,
`YOLO_INFER_EVERY_N`, `DEBUG`, `SIMULATION`, `SERIAL_BOOT`.

No test suite, no linter. Compiler strictness is the safety net (`-Werror=return-type`).
Pre-existing warning `yolo_v8n.cpp: iteration 1934464 invokes undefined behavior
[-Waggressive-loop-optimizations]` is the RGB-normalise tail loop over the
incomplete-array `test_image[]` — known, not yours.

### Unattended hardware bench — `make bench` (V183, `tools/HWBENCH.md`)

The fast loop: `make bench` resets the Pi over **RTS→RUN**, streams the ~60 KB code-only
`kernel8_serial.img` (`-DSERIAL_BOOT`) via the vendored `tools/raspbootin64/` stub, captures
UART, prints `[T]`, the `[P]` buckets, the **per-layer profile**, and diffs detections
against `GOLDEN` in `tools/hwbench.py`. FP32 weights + `test_image` live on the SD as
`d2m_data.bin` (`tools/pack_data.py`), loaded by the VPU at `0x08000000` via `initramfs`
(`build/config.serial.txt`). `SERIAL_BOOT` skips `camera_init()` (deterministic
`test_image`) and `#error`s with `USE_INT8*`. One-time SD prep: `make sdcard`. Reset is
RTS-only — never power-cycle (the CH340 re-enumerates). Do not pass `--reset-invert`.

Correctness regression for any kernel/graph change: on the last captured frame the
`(class, conf%)` set must equal `GOLDEN` — a real captured camera frame; V192 (full-FoV
320×192) → `[(74, 56)]` (the clock is small in the wide view — the `[ABS]` fingerprint is
the load-bearing check) — **and**
the `[ABS] <tag> amax1e3=` fingerprints (the `LOG_ABSMAX`
checkpoints, printed on the fp32 path only under `SERIAL_BOOT`) must match `GOLDEN_ABS`
within `--abs-tol`. Regenerate both from the SERIAL_BOOT build in QEMU (`-device
loader,file=d2m_data.bin,addr=0x08000000`, or `make bench BENCHFLAGS=--print-golden` on
HW) after model/exporter/test-image changes. **`test_image.bin` must be exactly 3·YOLO_W·YOLO_H floats** (737 280 B at 320×192). A size
mismatch misaligns the colour planes → garbage / `[DET] none`; check it first after any
geometry change. (V169–V183 shipped a 320² tensor to a 256² model with exactly this bug.)

**Live camera over the serial bench, no SD swap** (V199): the capture image is already
`SERIAL_BOOT` **with the camera on**, so setting `CAPTURE` out of reach turns it into a live
camera run streamed over UART — the frame dump never fires and it keeps inferring forever:

```bash
make CAPTURE=1000000 kernel8_capture.img
python3 tools/hwbench.py --port /dev/ttyUSB0 --kernel kernel8_capture.img \
        --frames 200 --timeout 700 --no-golden --log hwbench_cam.log
```

Use it for anything the deterministic bench cannot see: sustained clock behaviour on the
camera build, AE/AWB convergence, live `[DET]`, and `[PUMP] calls= paints=` (pump visits and
actual HUD repaints per inference frame — the objective form of "does the HUD still feel
smooth"). Detections will not match `GOLDEN`, hence `--no-golden`.

**Real camera frame → golden** (V185): `make capture` builds `kernel8_capture.img`
(`-DCAPTURE_FRAME=N`, camera ON, separate `cap_*.o`) and dumps the exact model input of frame
N over UART as base64 (`[CAP] begin/end`, watchdog kicked during the ~90 s dump);
`hwbench.py --capture cam_frame.bin` reassembles + CRC-checks it, then
`tools/capture_to_test_image.py` previews/installs it as `test_image.bin`. Regenerate blob +
goldens afterwards. No SD driver involved (`bsp/sdcard.cpp` is unlinked V116 code).

**Per-layer profiler** (`bsp/kernel.cpp`, declared in `bsp/bsp.h`): `prof_reset()` /
`prof_mark("name")` / `prof_dump()`. Marks are placed in `run_yolo_complete` after every
layer and inside `c2f_real_inference` (gated on prefix `"L6"`, macro `C2F_MARK`);
`prof_dump()` runs after `[T]` so marks never inflate `[P]`. Only meaningful on real HW.

## Regenerating model data

Weights + test image are `.incbin`'d via `app/yolo_v8n_coco/data.s` (4 blobs: fp32, int8,
w8a8, image). After touching the model or exporters:

```bash
python3 tools/convert_image.py                # -> app/<APP>/test_image.bin, [3][YOLO_H][YOLO_W]
                                               #    (Pillow + numpy; reads YOLO_W/YOLO_H from bsp/bsp.h)
tools/env/bin/python tools/export_model.py    # -> weights.bin + weights_crc.h   (needs torch)
tools/env/bin/python tools/export_int8.py     # -> weights_int8*.bin + *_crc.h   (only if INT8 matters)
make d2m_data.bin                              # -> d2m_data.bin for the bench (recopy to SD)
```

**`tools/env` is broken** (2026-09-03): `tools/env/bin/python{,3,3.13}` are 0-byte files
(dated 2026-07-08), so the torch-based exporters cannot run until the venv is recreated
(`python3 -m venv tools/env && tools/env/bin/pip install -r tools/requirements.txt`).
`convert_image.py` deliberately needs only system `python3`.

The kernel CRC-checks each blob at boot and halts on mismatch. Weight layout selection
(exporter ↔ `bsp/multicore.cpp` unpack) must stay in sync:
`K>1 && C_out>=32 && C_out%8==0` → `[C_out/8][C_in*K*K][8]`; `K>1 && C_out%4==0` →
`[C_out/4][…][4]`; else flat `[C_out][C_in*K*K]`.

## Architecture

```
bsp/       boot + hardware: start.s, kernel.cpp (uart, timer, profiler, kernel_main),
           mmu.cpp (1 GB identity, T0SZ=34), multicore.cpp (dispatch + conv2d kernels),
           mailbox.cpp/video.cpp (GPU FB), watchdog.cpp, sdcard.cpp (bare-metal SD),
           camera*.cpp + imx708_regs.h (IMX708 I2C → Unicam CSI-2 → debayer), bsp.h (contract)
runtime/   ops.cpp (conv1x1 8-ch + PRFM, upsample, maxpool5x5), ops_int8.cpp (W8A8, behind flags)
app/yolo_v8n_coco/  yolo_v8n.cpp (graph, C2f/SPPF, DFL decode, NMS, tracker, render), hud.cpp,
           data.s, weights*.bin, weights*_crc.h, test_image.bin, model_config.h
```

- **Boot (`bsp/start.s`)**: all 4 cores enter `_start`; core 0 clears BSS, sets `bss_ready`,
  drops EL2→EL1, enables NEON, per-core 64 KB stack + canary → `kernel_main`; cores 1–3 →
  `secondary_main`. `kernel_main` releases them via spin-table `0xE0/0xE8/0xF0`.
- **Per frame (`run_yolo_complete`)**: `[F<n>]` → CRC once → `bsp_frame_acquire` (if camera)
  → `input_img = g_use_camera ? cam_frame : test_image` → RGB normalise → L0..L8 backbone
  (C2f blocks, SPPF) → PAN neck (L12/L15/L16/L18/L19/L21) → 3 DFL heads (6 convs each) →
  NMS → `update_tracker()` → render + HUD → `[DET] id= c= %=` / `[P] cap= l0= bb= neck=
  head= render=` / `[T] <ms>ms`. Big activation buffers are static BSS (`buf_A/B/scratch`).
- **Multicore (`bsp/multicore.cpp`)**: master fills `parallel_task` (volatile), bumps
  `task_epoch` (RELEASE), `sev`; workers ACQUIRE-poll with exponential backoff, do their
  output-channel slice, RELEASE-add `done_count`; 50M-cycle timeout degrades to single-core.
  Kernels: `conv2d_partial` (4-ch), `conv2d_partial_8ch` (8-ch, C_out≥32) with B2 PRFM and
  the **P8 weight-stationary K=3 fast path** (ci outer, ci's weights read once per tile,
  1 KB tile accumulators). V183 gated it at whole-tile-interior; **V191 clips it to each
  tile's safe sub-rectangle** so only the 1-px padded ring uses the slow per-position path
  (was ~60 % of positions at S8=32); **V195 generalises that sub-rectangle to any
  `(stride, pad)`** — the six stride-2 conv3×3 (L1/L3/L5/L7/L16/L19, 87 ms) had no fast
  path at all — and moves the accumulator body into `p8_accum_{4,8}ch<S>`, `noinline`
  templates specialised on the stride. Both halves are bit-exact and worth ~35 ms each;
  the accumulator must stay a leaf — letting it inline costs 37 ms/frame. **V197 pads the
  input into a `(H+2)×(W+2)` scratch once per K=3/s1/p1 conv and runs it with `pad=0`**, so
  no position falls outside the fast path at all (the 1-px ring was 47 % of positions at S32,
  25 % at S16) — 435 → 367 ms. On HW the graph reaches these through
  `parallel_conv2d_pump_then` — **all four cores compute** and `display_pump()` runs between
  convs. The pump only does real work when a camera frame has just landed (~19 ms FSI period)
  and the graph runs ~63 convs/frame, so it repaints 10 times per inference (one per 29 ms)
  against 15 (one per 26 ms) for V180's continuous spin, while the frame is 90 ms faster —
  measured on HW with a live camera via `[PUMP] calls= paints=`. **V180's cores-1-3 async
  dispatch was deleted in V200**; there is now exactly one inference path. Its numbers live
  in PLAN.md V180/V198/V199/V200. `conv1x1` (`runtime/ops.cpp`): **V190 transposes each
  4-position input group** to fix an L1 set-thrash when HW is a multiple of 2048.
  `-DD2M_NO_P8` disables the conv2d fast path for A/B. QEMU never starts secondaries →
  single-core fallback.
- **Camera**: works end-to-end on HW since V164 (`[CAM] frame OK`, live detections).
  `camera_debug.md` + `camera_unicam.cpp` are the record; `hardware_sim.h` is the QEMU
  register oracle. Requires `dtoverlay=imx708` + `start_x=1` in `config.txt`.

## Verdicts already reached — don't re-propose

- **INT8 (W8A8) does not beat FP32 on this SoC** (GOALS.md, Tier 2 closed 2026-05-26): A53 is
  ARMv8.0 with no SDOT, so int8 MUL ≈ fp32 MUL; multicore int8 only matches multicore fp32;
  per-tensor quant is fragile vs camera activations. Kept behind `USE_INT8*` for a Pi 4/5
  port.
- **f32 conv3×3 is near the A53 NEON throughput limit** after P8. Micro-opts that measured
  **0 ms** on HW: contiguous output store vs `vgetq_lane` scatter; FMLA accumulator interleave
  (the A53 forwards the accumulate operand).
- **Winograd F(2×2,3×3) does not beat the P8 direct-conv kernel on this SoC** (PLAN.md V193):
  implemented + correctness-verified on HW (fingerprint 16/16 PASS) but measured a net
  **regression** — 492 → 521-523 ms (+6 %), unchanged after vectorizing the input-transform
  loads. Root cause: the M accumulator (16 taps × up to 8 tiles × 2 lo/hi = 256 `float32x4_t`)
  can't come close to living in the A53's 32 NEON registers, so almost every accumulate is a
  spill load + 1 FMA + spill store, vs the P8 kernel's 9-FMAs-per-register-round-trip
  weight-stationary design — the 9/4× fewer-FMA win is smaller than the ~9× worse
  load/store-to-FMA ratio. A real win needs a GEMM-style rewrite (tiles in the vector lanes,
  not the 8 output channels) — not attempted. Kept buildable behind `WINOGRAD=0` (default) /
  `make WINOGRAD=1` in `bsp/multicore.cpp` (`conv2d_winograd_tile_8ch` behind
  `#ifndef D2M_NO_WINOGRAD` — the whole function must not exist in the default build, or its
  stack frame taxes every `conv2d_partial_8ch` call even on the dead branch, ~12 ms/frame).
- B4 frame-skip raises throughput, not latency.
- **"Blown whites" on the camera were exposure, not white balance** (V188). Symptom: R/B at 1.0
  in 5–45 % of pixels while G looked unclipped. Grey-world AWB moved the gains by <5 % (the
  tuning WB was right for the room), and the AE grid showed ~half the sensor blocks ≥ 250 raw.
  G *was* saturated too — raw 255 − BLC 16 = 239 → gamma → ~247/255 = 0.97, just under a naive
  0.98 clip threshold — so per-channel "clipped" stats mislead here. The mean-only AE
  (`AE_TARGET=100` on raw MSB8) let high-contrast scenes burn; the fix is highlight protection
  in `ae_step()` (`AE_SAT_LEVEL`/`AE_SAT_MAX_PCT`), which prints `[AE] cit= mean= sat=`. Do
  not remove the post-WB clip in `isp_pixel` again: on sensor-saturated pixels it makes the
  burn redder.

## Conventions

- Comments/UART strings mix Spanish and English; match the surrounding file.
- QEMU (`raspi3b`, 62.5 MHz timer, no cache model) is a **correctness** check only; timing
  claims must come from real HW (19.2 MHz timer) via `make bench`. QEMU paths are gated on
  the timer frequency, not a build flag.
- Build artifacts are gitignored and untracked. `weights*.bin`, `test_image.bin` are tracked
  inputs. `.gitignore` also ignores `*.txt` — `tools/reference_output.txt` is tracked by hand.
- When you land a perf or bug fix, add a version-stamped entry to `PLAN.md` and, if it
  changes a goal's status, to `GOALS.md`.
