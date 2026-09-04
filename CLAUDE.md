# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**Direct2Metal** — YOLOv8n object detection (256×256, 80 COCO classes, anchor-free DFL)
running bare-metal on a Raspberry Pi Zero 2 W (BCM2837, 4× Cortex-A53). No OS: custom boot,
MMU, D-cache, 4-core dispatch, NEON kernels, and a working bare-metal MIPI CSI-2 driver for
the Pi Camera Module 3 (IMX708). Real-hardware results: **~518 ms / ~1.9 FPS** with live
camera detections at `YOLO_IN=192` (V164); **1276 ms/frame** at `YOLO_IN=256` on
`test_image`, 600 MHz low-power profile, fp32 (V183 bench — heads 34%, L2 C2f @S4 12%).
This tree is the live one; `~/projects/D2M` is a stale clone of the same GitHub repo (V76)
— do not develop there.

**The engineering logs are `GOALS.md` (roadmap + verdicts), `PLAN.md` (version log,
V1→V165) and `camera_debug.md` (Unicam bring-up).** `README.md` is a stale YOLOv5n copy —
trust GOALS/PLAN over it. Record new findings in GOALS.md / PLAN.md in the same
version-stamped style (next version after V181 is V183).

**Working tree has ~110 uncommitted files of the user's V180/V181 work.** Never
`git stash`, `git checkout -- .`, or `make clean`-style sweeps that could touch them. If you
must edit a dirty file, snapshot it first and keep your commit's hunks separate.

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
| `make FRAME_SKIP_N=4` | B4 frame-skip: infer 1 in N frames, re-render the rest. |
| `make DEBUG=1` | Camera thumbnail in the canvas. |
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
`(class, conf%)` set must equal `GOLDEN` **and** the `[ABS] <tag> amax1e3=` fingerprints
(the `LOG_ABSMAX` checkpoints, printed on the fp32 path only under `SERIAL_BOOT`) must
match `GOLDEN_ABS` within `--abs-tol`. `test_image.bin` gives no detections above
threshold, so the fingerprint is the check that actually bites. Regenerate both from the
SERIAL_BOOT build in QEMU (`-device loader,file=d2m_data.bin,addr=0x08000000`, or
`make bench BENCHFLAGS=--print-golden` on HW) after model/exporter/test-image changes.

**Per-layer profiler** (`bsp/kernel.cpp`, declared in `bsp/bsp.h`): `prof_reset()` /
`prof_mark("name")` / `prof_dump()`. Marks are placed in `run_yolo_complete` after every
layer and inside `c2f_real_inference` (gated on prefix `"L6"`, macro `C2F_MARK`);
`prof_dump()` runs after `[T]` so marks never inflate `[P]`. Only meaningful on real HW.

## Regenerating model data

Weights + test image are `.incbin`'d via `app/yolo_v8n_coco/data.s` (4 blobs: fp32, int8,
w8a8, image). After touching the model or exporters:

```bash
tools/env/bin/python tools/export_model.py    # -> weights.bin + weights_crc.h
tools/env/bin/python tools/export_int8.py     # -> weights_int8*.bin + *_crc.h  (only if INT8 matters)
tools/env/bin/python tools/convert_image.py   # -> test_image.bin (CHW float32)
make d2m_data.bin                              # -> d2m_data.bin for the bench (recopy to SD)
```

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
  the **V183/P8 weight-stationary K=3/s1/p1 fast path** (ci outer, 1 KB tile accumulators —
  interior tiles only). QEMU never starts secondaries → single-core fallback.
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
  (the A53 forwards the accumulate operand). Remaining algorithmic lever: Winograd F(2×2,3×3).
- B4 frame-skip raises throughput, not latency.

## Conventions

- Comments/UART strings mix Spanish and English; match the surrounding file.
- QEMU (`raspi3b`, 62.5 MHz timer, no cache model) is a **correctness** check only; timing
  claims must come from real HW (19.2 MHz timer) via `make bench`. QEMU paths are gated on
  the timer frequency, not a build flag.
- Build artifacts are gitignored and untracked. `weights*.bin`, `test_image.bin` are tracked
  inputs. `.gitignore` also ignores `*.txt` — `tools/reference_output.txt` is tracked by hand.
- When you land a perf or bug fix, add a version-stamped entry to `PLAN.md` and, if it
  changes a goal's status, to `GOALS.md`.
