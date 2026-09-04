# Direct2Metal — YOLOv5n Bare-Metal on Raspberry Pi Zero 2 W

> **This README is a stale YOLOv5n-era snapshot.** The project is now **YOLOv8n 256²**
> with a working IMX708 camera pipeline (V164+). The maintained engineering logs are
> **`GOALS.md`** (roadmap + verdicts, incl. why INT8 was closed), **`PLAN.md`** (version
> log — see the **V183** row) and `camera_debug.md`. Current real-HW figure (V183, 600 MHz
> low-power profile, `test_image`, fp32): **1276 ms/frame**; heads = 34% of that.
> Iterate with the unattended bench: `make bench` (`tools/HWBENCH.md`). Onboarding for
> Claude Code lives in `CLAUDE.md`.

**Hypothesis:** A real-time object detection model runs significantly faster without an operating system, eliminating scheduler overhead, generic drivers, and high-level abstraction layers.

**Approach:** Port YOLOv5n (320×320 input, 80 COCO classes) to bare-metal AArch64 using ARM NEON SIMD, 4-core parallelism, and a custom MMU with D-cache, running on a Raspberry Pi Zero 2 W with no OS of any kind.

---

## Current Result: 511 ms / ~2 FPS (real hardware)

```
Clase: 0 | Conf: 88 | Pos: [54,189]
Clase: 5 | Conf: 85 | Pos: [161,147]
Clase: 0 | Conf: 82 | Pos: [111,187]
Clase: 0 | Conf: 74 | Pos: [296,183]
>> Tiempo RGB:              4 ms
>> Tiempo L0 (K=6):        43 ms
>> Tiempo Backbone L1-L8: 272 ms
>> Tiempo Neck y Heads:   136 ms
>> Tiempo Decode + NMS:    22 ms
TOTAL TIME: 511 ms
```

---

## Performance Summary

### YOLOv5n 320×320 — Real Hardware Evolution

| Milestone | Configuration | Time | FPS |
|:---|:---|:---:|:---:|
| Python / ONNX Runtime (Linux) | OS baseline | ~1,300 ms | 0.77 |
| Bare-metal scalar C++ + D-cache | Single core | 1,906 ms | 0.52 |
| 4-core NEON, secondary cores uncached | Phase 6 (bug) | 4,199 ms | 0.24 |
| 4-core NEON, all cores cached | Phase 7 fix | 598 ms | 1.67 |
| + ISO 26262 safety + NMS fix + maxpool NEON | Phase 8 | 598 ms | 1.67 |
| **+ 8-channel conv3×3 kernel** | **Current** | **511 ms** | **~2** |

> The Phase 6 regression (4,199 ms > 1,906 ms scalar) happened because cores 1–3 ran without D-cache: every weight load hit uncached LPDDR2 (~10–50× slower), making Core 0 wait for three slow workers.

### Per-bucket breakdown — current build vs 4-ch baseline

| Bucket | 4-ch baseline | 8-ch current | Δ |
|:---|:---:|:---:|:---:|
| RGB normalisation | 5 ms | 4 ms | −1 ms |
| L0 stem (K=6, C_out=16) | 43 ms | 43 ms | — (4-ch unchanged) |
| Backbone L1–L8 | 333 ms | 272 ms | **−61 ms (1.22×)** |
| Neck + detection heads | 161 ms | 136 ms | **−25 ms (1.18×)** |
| Decode + NMS + UART + draw | 23 ms | 22 ms | −1 ms |
| **Total** | **598 ms** | **511 ms** | **−87 ms** |

### TinyYOLO 64×64 (Phase 3–5, historical)

| Configuration | Total | FPS |
|:---|:---:|:---:|
| Scalar C++ + D-cache | 222 ms | 4.5 |
| NEON 4-core + D-cache | **75 ms** | **13** |

---

## Project Phases

| Phase | Description | Key result |
|:---:|:---|:---|
| 1 | NEON GEMM 16×16 (assembly) | 2.52× over scalar |
| 2 | NEON Conv2d (assembly) | 1.75× over scalar |
| 3 | TinyYOLO 64×64 pipeline, QEMU validated | <1e-4 error vs PyTorch |
| 4 | MMU + D-cache (T0SZ=34, identity map 1 GB) | 222 ms / 4.5 FPS |
| 5 | 4-core parallel dispatch (task_epoch / done_count atomics) | 75 ms / 13 FPS |
| 6 | YOLOv5n 320×320: NEON SiLU, atomic sync | 4,199 ms — D-cache bug found |
| 7 | `init_mmu()` in `secondary_main()` — all cores fully cached | 598 ms / 1.67 FPS |
| 8 | ISO 26262 safety hardening + NMS fix + NEON maxpool + 8-ch conv | **511 ms / ~2 FPS** |
| 9 | Pi Camera v3 (IMX708) bare-metal CSI-2 driver | **V28: sensor 55fps confirmed; D-PHY CLK frozen — testing clock gate + CLE** |

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  YOLOv5n Engine  (C++ / ARM NEON)    320×320 input           │
│  Backbone: Focus-like stem → C3 blocks → SPPF               │
│  Neck: PAN-FPN with upsample + concat                        │
│  Head: 3 detection scales (40×40, 20×20, 10×10)             │
├──────────────────────────────────────────────────────────────┤
│  Multicore Orchestrator  (src/multicore.cpp)                 │
│  Core 0: master   Cores 1-3: workers (all with D-cache)     │
│  Protocol: task_epoch (RELEASE) + done_count (ACQUIRE) + wfe │
│  QEMU fallback: 500K-cycle probe → single-core if no workers │
│  8-ch kernel selected when C_out ≥ 32 && C_out % 8 == 0     │
├──────────────────────────────────────────────────────────────┤
│  MMU + D-Cache + I-Cache  (src/mmu.cpp)                      │
│  Identity map 1 GB, T0SZ=34 (512 × 2 MB blocks)            │
│  RAM: Normal WB Inner Shareable  │  MMIO: Device-nGnRnE      │
│  All 4 cores independently enable their own MMU registers    │
├──────────────────────────────────────────────────────────────┤
│  BCM2837 — 4× Cortex-A53 @ 1.2 GHz — 512 MB LPDDR2         │
└──────────────────────────────────────────────────────────────┘
```

### Software stack

```
[PyTorch on host PC]
    │  export_model.py → repack weights:
    │    C_out ≥ 32 && C_out%8 == 0 → [C_out/8][C_in×K×K][8]  (8-ch)
    │    C_out%4 == 0               → [C_out/4][C_in×K×K][4]  (4-ch)
    ↓
[weights.bin + test_image.bin] ──incbin──→ [.rodata in kernel8.img]
    ↓
[start.s] → EL2→EL1 (all cores), CPACR.FPEN=0b11, BSS clear → kernel_main()
    ↓
[mmu.cpp::init_mmu()] → 1GB identity table, T0SZ=34, MMU+D$+I$ ON (all 4 cores)
    ↓
[kernel.cpp::run_yolo_complete()]
    ├── CRC32 integrity check on weights.bin  (S8)
    ├── watchdog_kick()                        (S7, real HW only)
    ├── parallel_conv2d / parallel_conv1x1  →  [multicore.cpp: 4-core dispatch]
    │       ├── conv2d_partial_8ch()         →  [K=3, C_out≥32: 18 vmlaq per ci]
    │       └── conv2d_partial()             →  [K=3/K=6, C_out<32: 9 vmlaq per ci]
    ├── silu_inplace()                       →  [ops.cpp: neon_expf4 + Newton-Raphson]
    ├── upsample2x_nearest / maxpool5x5_s1_p2
    ├── decode_yolo_grid() × 3 scales        →  sigmoid, anchor decode
    └── insertion-sort NMS (IoU threshold 0.35)
    ↓
[UART] → timing breakdown + detected objects
[HDMI framebuffer] → raw image + bounding boxes
```

---

## Phase 8 — Safety & Optimisation Details

### ISO 26262 Safety Features

| ID | Feature | File | Description |
|:---:|:---|:---|:---|
| S1 | Named constants | `safety_config.h` | Eliminates magic numbers; single source of truth for thresholds and geometry |
| S2 | `volatile` task fields | `multicore.h` | Prevents compiler from caching shared task fields across memory barriers |
| S3 | Worker timeout | `multicore.cpp` | 50M-cycle countdown in `wait_for_workers()`; degrades to single-core on expiry |
| S4 | NULL guard | `multicore.cpp` | `flush_to_ram()` early-returns on `addr == nullptr` or `size == 0` |
| S5 | Float equality fix | `kernel.cpp` | `conf == 0.0f` → `conf <= 0.0f` (avoids UB with −0.0f) |
| S6 | Stack canaries | `start.s` + `multicore.cpp` | `0xDEADBEEFDEADBEEF` written at each core's stack bottom at boot; checked before every dispatch |
| S7 | Hardware watchdog | `watchdog.cpp` | BCM2835 PM watchdog at `0x3F100000`, 4 s timeout, kicked every frame; skipped on QEMU |
| S8 | Weights CRC32 | `kernel.cpp` + `export_model.py` | Software CRC32 verified once at startup; `SAFETY_HALT` on mismatch |
| S9 | ISB after SPSR write | `start.s` | `isb` before `eret` per ARM DDI0487 D1-3449 |
| S10 | Strict compiler flags | `Makefile` | `-Werror=implicit-function-declaration -Werror=return-type` |

### Performance Optimisations (Phase 8)

| ID | Feature | File | Gain |
|:---:|:---|:---|:---|
| P1 | Insertion sort NMS + early exit | `kernel.cpp` | Faster NMS; fixes early-termination bug causing double detections |
| P2 | NEON maxpool5×5 | `ops.cpp` | `vmaxq_f32` on 4 of 5 window values per row for interior pixels |
| P3 | Scalar class argmax | `kernel.cpp` | Reverted NEON scatter-gather (CHW stride kills benefit on A53 in-order pipeline) |
| P4 | NEON RGB normalisation | `kernel.cpp` | `vld1q_f32` + `vmulq_n_f32` × 4 pixels per iteration |
| P5 | Exponential backoff in worker spin | `multicore.cpp` | Starts at 10 yields, doubles to 1024; reduces wasted cycles on short waits |
| P6 | p_step-outer loop in conv1x1 | `ops.cpp` | Input tile (C_in × 32 pixels) loaded once and reused across all output-channel groups |
| P7 | 8-channel conv3×3 kernel | `multicore.cpp` | Two `float32x4_t` accumulators per pixel; 18 `vmlaq_n_f32` per ci vs 9 → 2× FMA/load |

### NMS Bug Fix

The Phase 8 insertion-sort NMS contained a subtle early-exit bug: the inner suppression loop `break`-ed when it encountered a previously suppressed box (`conf == 0 < CONF_THRESH × 0.5`), stopping before checking remaining valid boxes behind it. The fix adds `if (preds[jj].conf <= 0.0f) continue;` before the break condition. Additionally, `NMS_THRESH` was lowered from `0.45` to `0.35` to suppress different-anchor boxes at the same spatial position (IoU ≈ 0.41).

---

## Project Structure

```
src/
├── start.s              — Boot entry: EL2→EL1, NEON enable, bss_ready, per-core stacks + canary write
├── kernel.cpp           — Main loop: YOLOv5n layers, NMS, HDMI rendering, watchdog kick, CRC check
├── multicore.cpp        — 4-core dispatch: conv2d_partial_8ch (C_out≥32) + conv2d_partial (C_out<32)
├── multicore.h          — ParallelTask struct (volatile fields), TASK_CONV2D / TASK_CONV1X1
├── ops.cpp              — NEON kernels: conv1x1, SiLU, upsample, maxpool5x5_s1_p2
├── ops.h                — Operator declarations
├── safety_config.h      — Named constants (thresholds, geometry) + SAFETY_ASSERT / SAFETY_HALT macros
├── watchdog.cpp/h       — BCM2835 PM watchdog (0x3F100000); watchdog_init() + watchdog_kick()
├── mmu.cpp              — MMU + D-cache init; mmu_table_ready sync for secondary cores
├── mmu.h
├── mailbox.cpp          — GPU mailbox: framebuffer allocation, dc civac flush, 0xC0000000 bus alias
├── mailbox.h
├── video.cpp            — Framebuffer draw primitives (boxes, text)
├── conv2d.cpp           — Dispatch wrapper
├── conv2d_neon.s        — Legacy NEON conv2d (assembly, Phase 2)
├── data.s               — .incbin of weights.bin + test_image.bin → .rodata
├── weights.bin          — YOLOv5n weights: 8-ch for K>1 C_out≥32, 4-ch otherwise (7.5 MB)
├── weights_crc.h        — Auto-generated by export_model.py: WEIGHTS_CRC32, WEIGHTS_SIZE
├── test_image.bin       — 320×320 test image, CHW float32 (1.2 MB)
├── linker.ld            — Loads at 0x80000; .rodata, .data, .bss
│
│   ── Phase 9: Camera ──────────────────────────────────────────────────────
├── camera.h             — Public API: camera_init(), camera_capture_frame(), g_use_camera
├── camera.cpp           — Orchestration: IMX708 + Unicam + debayer; CE→stream_on→CPE sequence
├── camera_bsc.cpp       — BCM2837 BSC1 I2C driver; GPIO40/41/42/44 power/reset sequence
├── camera_imx708.cpp    — IMX708 sensor: probe, register injection (~135 regs), stream on/off
├── camera_unicam.cpp    — Unicam CSI-2: DMA buffer, ANA=0x43, ICTL/ISTA polling, dc ivac
├── camera_debayer.cpp   — RAW8 RGGB → center-crop 864×864 → 320×320 NEON float32
└── imx708_regs.h        — Register tables: k_imx708_common[] (mode_common+link_450MHz) + k_imx708_init[]

tools/
├── export_model.py      — Exports YOLOv5n from PyTorch; generates weights.bin + weights_crc.h
├── convert_image.py     — Converts arbitrary image to test_image.bin format
├── get_regs.py          — Extracts IMX708 register tables from Linux driver source
└── benchmark_pytorch.py

build/
└── config.txt           — RPi firmware config (arm_64bit, UART, HDMI, start_x=1, dtoverlay=imx708)
```

---

## Build

### Prerequisites

- Docker image `rpi-forge` (contains `aarch64-linux-gnu-g++` cross-compiler)
- Python 3 + PyTorch (to regenerate weights; a virtualenv is provided at `tools/env/`)

### Steps

```bash
# 1. Export weights (required after any change to export_model.py or the model)
tools/env/bin/python tools/export_model.py

# 2. Build the bare-metal kernel
docker run --rm -v $(pwd):/app rpi-forge make kernel8.img

# 3. Smoke-test in QEMU (single-core fallback — correct for QEMU raspi3b)
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none
```

Expected QEMU output (single-core, ~1 s — no cache model, so timing is not representative):

```
=== Direct2Metal: MOTOR IA EN TIEMPO REAL ===
...
>>> OBJETOS DETECTADOS <<<
Clase: 0 | Conf: 88 | Pos: [54,189]
Clase: 5 | Conf: 85 | Pos: [161,147]
Clase: 0 | Conf: 82 | Pos: [111,187]
Clase: 0 | Conf: 74 | Pos: [296,183]
TOTAL TIME: ~1000 ms    ← single-core QEMU; real HW multi-core = 511 ms
```

> QEMU always shows single-core timing because `raspi3b` emulation does not start secondary cores. On real hardware all 4 cores run fully cached, yielding the 511 ms figure above.

---

## Flashing to Hardware

| File | Source | Notes |
|:---|:---|:---|
| `bootcode.bin`, `start.elf`, `fixup.dat` | RPi firmware repo | Must already be on the SD card |
| `config.txt` | `build/config.txt` in this repo | Sets 64-bit mode, UART, HDMI |
| `kernel8.img` | Built by `make kernel8.img` | Our bare-metal kernel |

```bash
cp kernel8.img /media/$USER/bootfs/
sync
```

### Key `config.txt` settings

```ini
arm_64bit=1           # Required: load kernel8.img as AArch64
kernel=kernel8.img    # Tell firmware to load our kernel
enable_uart=1         # PL011 UART on GPIO14/15 (TX/RX) for serial debug
dtoverlay=disable-bt  # Reassign UART from Bluetooth to GPIO pins
gpu_mem=128           # Reserve 128 MB for GPU/framebuffer + camera DMA
hdmi_force_hotplug=1  # Force HDMI even without a monitor detected
start_x=1             # Load start_x.elf — camera-capable VPU firmware
dtoverlay=imx708      # REQUIRED for camera: calibrates CSI-1 D-PHY analog frontend,
                      # configures GPIO43 (GPCLK2) as 24 MHz sensor MCLK,
                      # and sets GPIO42 as camera 2.8 V power regulator enable
```

> `dtoverlay=imx708` is mandatory even in bare-metal. It runs at VPU firmware level before the ARM core starts and handles the D-PHY analog calibration that cannot be replicated from bare-metal code.

### Serial debug

Connect a USB-UART adapter to **GPIO14 (TX)** / **GPIO15 (RX)** at **115200 baud 8N1**.

---

## Detection Configuration

| Parameter | Value | Notes |
|:---|:---:|:---|
| Input resolution | 320×320 | CHW float32, normalised [0,1] |
| Confidence threshold | 0.5 | Score = obj_conf × max_class_prob |
| NMS IoU threshold | 0.35 | Lowered from 0.45 to suppress different-anchor same-position boxes (IoU ≈ 0.41) |
| Objectness pre-filter | 0.45 | Early discard before class decode |
| Max predictions (pre-NMS) | 100 | Hard cap |
| Anchors P3 (stride 8, 40×40) | [10,13], [16,30], [33,23] | Standard YOLOv5n, input-image pixel units |
| Anchors P4 (stride 16, 20×20) | [30,61], [62,45], [59,119] | |
| Anchors P5 (stride 32, 10×10) | [116,90], [156,198], [373,326] | |
| COCO classes | 80 | Class 0 = person, class 5 = bus |

> **Anchor note**: `anchor_grid = anchors × stride` is always in input-image pixel space regardless of training resolution. Do **not** rescale anchors by resolution ratio.

---

## Synchronisation Protocol (multicore)

```
Core 0                          Cores 1-3 (secondary_main)
───────────────────────────     ──────────────────────────────────────
[start.s: EL2→EL1, NEON]       [start.s: EL2→EL1, NEON, per-core stack]
kernel_main():                  secondary_main():
  bss_ready = 1 + sev ───────►    init_mmu() → wait mmu_table_ready
  video_init()                                → enable own MMU+cache
  init_mmu():                     __atomic_add(cores_ready)
    fill table                    loop:
    mmu_table_ready=1+sev ─────►    ACQUIRE-poll task_epoch (exp. backoff)
    enable own MMU+cache            read task snapshot
  while(1):                         select 8-ch or 4-ch kernel from C_out
    run_yolo_complete()             execute assigned slice
      parallel_conv2d():            RELEASE-add done_count + sev
        probe (500K yield) ────►  cores_ready==3 ✓
        fill parallel_task
        done_count = 0
        dsb ish
        RELEASE-add task_epoch
        sev ──────────────────►   wake, run slice (fully cached)
        do own slice (0..n/4)
        wait done_count==3 ◄────  done
        dsb ish
```

QEMU fallback: on the first `parallel_conv2d` call, Core 0 spins 500K cycles waiting for `cores_ready == 3`. If fewer than 3 secondary cores respond (QEMU never starts them), `multicore_available = 0` and all subsequent calls run single-core.

---

## 8-Channel Conv3×3 Kernel (P7)

The key optimisation in the current build doubles FMA throughput per memory access for all backbone and neck K=3 convolutions.

### Weight layout

| Condition | Layout | Groups |
|:---|:---|:---|
| `K > 1`, `C_out ≥ 32`, `C_out % 8 == 0` | `[C_out/8][C_in×K×K][8]` | 8-ch (backbone, neck) |
| `K > 1`, `C_out % 4 == 0` | `[C_out/4][C_in×K×K][4]` | 4-ch (L0 stem, C_out=16) |
| otherwise | `[C_out][C_in×K×K]` | flat (1×1 heads) |

### K=3 safe path — per output pixel, per group

```
4-ch (9 scalar loads, 9 vmlaq_n_f32):
  for ci: ip[0..8] → 9 × vmlaq_n_f32(acc, w[pos*4], ip[pos])
  Result: 4 channels in acc

8-ch (9 scalar loads, 18 vmlaq_n_f32):
  for ci: ip[0..8] → 9 × { vmlaq_n_f32(acc_lo, w[pos*8],   ip[pos])
                           vmlaq_n_f32(acc_hi, w[pos*8+4], ip[pos]) }
  Result: 8 channels in acc_lo + acc_hi
```

Same 9 scalar loads, 2× FMA output. The 1.22× measured speedup vs theoretical 2× reflects the LPDDR2 bandwidth limit on the 7.5 MB weight tensor.

---

## HDMI / Video Stability Notes

Running all 4 Cortex-A53 cores at 100% saturates the BCM2837 shared memory bus. The VideoCore IV GPU competes for the same bus to fetch the framebuffer for HDMI output.

**Mitigations applied:**
- `dc civac` flush + `0xC0000000` bus alias in `mailbox.cpp` to ensure GPU-visible writes
- `flush_to_ram()` called after every frame draw
- `hdmi_force_hotplug=1` in `config.txt`

**Known issue:** at full 4-core load, the display may lose sync briefly on slower monitors. Adding `force_turbo=1` to `config.txt` locks the memory controller clock and eliminates this. Not set by default to avoid thermal throttling on passive cooling.

---

## Key Bugs Solved

| Bug | Symptom | Root cause | Fix |
|:---|:---|:---|:---|
| Silent hang after `init_mmu()` | No UART output | `T0SZ=25` maps MMIO as Normal WB cached; UART writes go to L1, never reach hardware | `T0SZ=34` → Level-2 walk, MMIO as Device-nGnRnE |
| Secondary cores slower than single-core | 4,199 ms > 1,906 ms scalar | `init_mmu()` never called in `secondary_main()`; cores 1–3 hit uncached LPDDR2 on every weight load | Call `init_mmu()` at start of `secondary_main()` before incrementing `cores_ready` |
| Double detections on real HW | Same object detected 2–4× at identical position | NMS inner loop `break` fired on suppressed boxes (conf=0 < threshold), halting before checking remaining valid boxes | Added `if (preds[jj].conf <= 0.0f) continue;` before the break; lowered NMS_THRESH 0.45 → 0.35 |
| Bounding boxes drawn as lines | Invisible boxes on HDMI | Anchors incorrectly halved — `anchor_grid = anchors × stride` is in input-pixel space regardless of resolution | Reverted to standard YOLOv5n anchor values |
| GPU mailbox sees zeros on real HW | Blank display | D-cache traps CPU writes; GPU DMA reads stale DRAM | `dc civac` flush + `0xC0000000` bus alias in `mailbox.cpp` |
| QEMU watchdog reset loop | System resets immediately | `watchdog_kick()` with `g_wdog_ticks=0` arms a 0-tick timeout → instant reset | Guard `g_watchdog_armed` in kick(); skip `watchdog_init()` on QEMU (`cntfrq_el0 == 62500000`) |
| P6 loop inversion regression | 615 ms on HW (worse than 598 ms baseline) | Co-outer loop in conv1x1 caused cache thrashing: 409 KB input buffer re-read 64× per frame for 256-out layers | Reverted to p_step-outer: input tile (C_in × 32 pixels) loaded once, reused by all output-channel groups |

---

## Historical Benchmarks

All rows marked "HW" measured on real RPi Zero 2 W (ARM Generic Timer, 19.2 MHz). QEMU rows use the 62.5 MHz virtual timer and do not model cache.

| ID | Test | Configuration | Result |
|:---:|:---|:---|:---:|
| B01 | GEMM 16×16 | Scalar C++ -O3 | 16,106 cycles |
| B02 | GEMM 16×16 | NEON assembly | 6,383 cycles (2.52×) |
| AI-01 | Conv2d 16×16 | Scalar C++ | 91,135 cycles |
| AI-02 | Conv2d 16×16 | NEON (copy-then-compute) | 186,944 cycles (regression) |
| AI-03 | Conv2d 16×16 | NEON direct | 51,933 cycles (1.75×) |
| NN-01 | TinyYOLO 64×64 | Scalar + D-cache (HW) | 222 ms / 4.5 FPS |
| NN-02 | TinyYOLO 64×64 | 4-core NEON + D-cache (HW) | 75 ms / 13 FPS |
| YV-01 | YOLOv5n 320×320 | Scalar + D-cache (HW) | 1,906 ms / 0.52 FPS |
| YV-02 | YOLOv5n 320×320 | 4-core NEON, secondary uncached (HW) | 4,199 ms (bug) |
| YV-03 | YOLOv5n 320×320 | 4-core NEON, all cached, Phase 7 (HW) | 598 ms / 1.67 FPS |
| YV-04 | YOLOv5n 320×320 | + Phase 8 safety + NMS fix (HW) | 598 ms / 1.67 FPS |
| **YV-05** | **YOLOv5n 320×320** | **+ 8-ch conv3×3 kernel (HW)** | **511 ms / ~2 FPS** |

---

---

## Phase 9 — Pi Camera v3 (IMX708) Bare-Metal CSI-2 Driver

### Goal

Replace the static `test_image.bin` with live frames from the Pi Camera Module 3 (Sony IMX708, 12 MP). The camera outputs 1536×864 RAW8 RGGB via MIPI CSI-2 at 2 lanes, then debayered and center-cropped to 320×320 for the YOLO pipeline.

### New source files

| File | Role |
|:---|:---|
| `src/camera.h` | Public API: `camera_init()`, `camera_capture_frame(float*)`, `g_use_camera` |
| `src/camera.cpp` | Top-level orchestration: I2C + Unicam + debayer; CE→stream_on→CPE sequence |
| `src/camera_bsc.cpp` | BCM2837 BSC1 I2C driver; GPIO reset/power sequence; GPCLK0 setup |
| `src/camera_imx708.cpp` | IMX708 probe (chip ID 0x0708), register injection (~135 regs), stream on/off |
| `src/camera_unicam.cpp` | BCM2837 Unicam CSI-2 receiver; DMA buffer `g_raw_frame[1536×864]`; frame polling |
| `src/camera_debayer.cpp` | RAW8 RGGB → center-crop 864×864 → nearest-neighbour scale to 320×320, NEON float32 |
| `src/imx708_regs.h` | Auto-generated register tables: `k_imx708_common[]` + `k_imx708_init[]` |

### Key discoveries (from iterative hardware testing)

#### GPIO map — Pi Zero 2W camera FFC (confirmed via GPFSEL4 readback with `dtoverlay=imx708`)

| GPIO | Function | Notes |
|:----:|:---------|:------|
| 2 | BSC1 SDA (ALT0) | I2C camera data |
| 3 | BSC1 SCL (ALT0) | I2C camera clock |
| 40 | OUTPUT | Camera power enable (some board revisions) |
| 41 | OUTPUT | XSHUTDOWN — HIGH = active |
| 42 | OUTPUT | Camera 2.8 V analog power regulator enable |
| 43 | GPCLK2 (ALT0) | **Actual sensor MCLK** (24 MHz, set by `dtoverlay=imx708`) |
| 44 | OUTPUT | Camera reset |
| 45 | BSC1 SCL (ALT2) | Alternate I2C SCL on this pin |

> **Critical**: on Pi Zero 2W the FFC MCLK trace is wired to **GPIO43 (GPCLK2)**, not GPIO34 (GPCLK0) as on Pi 3/4. `dtoverlay=imx708` configures this automatically; without the overlay, the IMX708 MIPI PLL has no clock and the D-PHY stays silent.

#### Unicam init order matters — must follow Linux driver sequence

```
unicam_init()       → CTRL=0x01  (CE only — D-PHY active, no protocol decoding)
imx708_stream_on()  → 0x0100=0x01
unicam_enable_cpe() → CTRL=0x05  (CE | CPE — AFTER sensor streams)
delay 500 ms
```

Enabling CPE before `stream_on` desynchronises the protocol engine and produces ISTA=0 regardless of physical MIPI activity.

#### ANA register — D-PHY analog bias

The VPU leaves ANA=**0x777** (confirmed via pre-init dump in V26), which includes all calibration bits. **Do not write ANA** from bare-metal code — doing so risks re-triggering DDL calibration and overwriting VPU's bias values. An earlier version wrote ANA=0x43 explicitly; this was a no-op since 0x777 is a superset of 0x43, but the write itself was unnecessary and potentially harmful.

#### Software reset omitted

The Linux `imx708` driver does **not** issue a software reset (reg 0x0103=0x01) before writing mode registers. After XSHUTDOWN power-on reset the sensor is in a clean default state; a software reset can push the internal MIPI PLL into an undefined state. Our init sequence follows this.

#### `mode_common_regs[]` is mandatory

`get_regs.py` initially extracted only `mode_2x2binned_720p_regs[]`. The Linux driver writes `mode_common_regs[]` + `link_450MHz_regs[]` **first**. Missing registers included:
- `0x3062–0x3079` — sensor-internal CSI-2 PHY configuration (**critical for MIPI TX**)
- `0xF001–0xF03F` — undocumented internal calibration
- `0x030E/0x030F` — OP-PLL multiplier (link 450 MHz)

### Current status — V28

| Subsystem | Status | Notes |
|:---|:---:|:---|
| I2C (BSC1) | ✅ | GPIO 44/45 ALT2, chip ID 0x0708 confirmed |
| IMX708 registers | ✅ | ~135 registers injected; RAW8 + 2-lane MIPI confirmed via readback |
| IMX708 streaming | ✅ | 0x0100 readback = 0x01; frame_count=55 at t=500ms → 55fps MIPI confirmed (V25) |
| Sensor MCLK | ✅ | V24 fix: CM_GP2 → SRC=PLLD, DIVI=20 → **24 MHz** (dtoverlay misconfigured to 32.6 kHz) |
| GPIO42 analog power | ✅ | LOW → HIGH with GPIO40/41/44 at power-on |
| Unicam clocks | ✅ | CM_CAM1CTL: MASH=1, DIVI=2, SRC=PLLD → ~250 MHz receiver clock |
| D-PHY calibration | ✅ | `dtoverlay=imx708` triggers VPU CSI-1 analog frontend (ANA=0x777) |
| DMA routing | ✅ | `0x40000000` ARM AXI L2-bypass; 0xC0000000 GPU bus = silent fail (V18) |
| LSM (lane swap) | ✅ | **LSM=1 REQUIRED** — D0/D1 physically crossed on Pi Zero 2W FFC |
| ICTL | ✅ | ICTL=0x00 required; ICTL=0x03 kills DMA engine (V21 proven) |
| D-PHY CLK lock | ❌ | CLK=0xCCC00002 frozen PRE=POST stream; D-PHY not receiving HS |
| Complete frame (ISTA_FE) | ❌ | V28 testing: correct clock gate 0x3F802004 + CLE bit |

### V17 → V21 root cause analysis

**V17 — WP advances +4096 bytes, ISTA=0 always**

`LSM=1`, `IDI0=0x00`, `ICTL=0x00`. The Unicam receives ~4096 bytes from the sensor (WP moves). But `ISTA` stays 0 for the entire 30 M-iteration poll (~500 ms). Possible causes: (A) `ICTL=0x00` prevents ISTA events from latching; (B) the 4096 bytes are embedded metadata (DT≠0x2A) and the image burst never completes.

**V18 — DMA completely dead (WP = IBSA0)**

V18 changed `LSM=1→0` (wrong hypothesis: "LSM corrupts 2-lane header") and introduced two new bugs:

| Bug | V18 value | Effect |
|:---|:---:|:---|
| Wrong bus alias | `0xC0000000` | VideoCore/GPU bus space, not ARM AXI. Unicam writes to an invalid address → DMA fails silently. |
| IPIPE=0 | `0` | "straddle mode" disabled — no data routed to DMA buffer regardless of address. |

Serial: `WP0=-1037831808` = `0xC2620000` = exact IBSA0 with `0xC0` alias (no movement).

**V19 — DMA still dead (WP=IBSA0)**

Fixed bus alias and IPIPE, but kept `LSM=0`. Result: WP=IBSA0.
Serial: `WP0=1109651840` = `0x42200780` = exact `0x40000000|phys_addr` = IBSA0 (no movement).

**LSM=1 is physically required on Pi Zero 2W** (proven by WP comparison):
The Pi Zero 2W FFC has MIPI D0/D1 physically swapped between IMX708 and BCM2837 CSI-1. LSM=1 corrects this.
- `V17 (LSM=1)`: WP +4096 bytes → D-PHY clock recovery working → lanes live
- `V18/V19 (LSM=0)`: WP=IBSA0 → no D-PHY activity → lane swap kills clock recovery

**V20 — IDI0=0x2A backfires (WP=IBSA0)**

Theory: IMX708 sends embedded lines (DT≠0x2A) first; IDI0=0x2A would filter them and only pass RAW8 → ISTA_FE fires cleanly.
**Result: WP=IBSA0**, identical to V18/V19.
**Conclusion: IDI0=0x2A filtered ALL packets** — the sensor was sending packets that are NOT DT=0x2A at the time of the V17 capture (likely embedded data DT=0x12/0x13 only). Image lines (DT=0x2A) were either never arriving at DMA or arriving only after the embedded burst. IDI0=0x2A blocked everything.

**V21 — ICTL=0x03 hypothesis**

Result: WP=IBSA0, ICTL readback=0. Writing ICTL=0x03 disrupted the DMA engine — ICTL acts as a trigger, not a persistent enable. Reverted to ICTL=0x00.

**V22 — All registers correct, CLK frozen**

Post-CPE dump confirmed: CTRL=0x4005 ✓, IDI0=0 ✓, IPIPE=1 ✓, ICTL=0 ✓, IBSA0=correct ✓. But PRE-STREAM==POST-STREAM for CLK/D0/D1 → D-PHY not receiving MIPI. Root cause: MCLK.

**V23/V24 — MCLK root cause found and fixed**

`dtoverlay=imx708` sets CM_GP2: SRC=XOSC(19.2 MHz), DIVI=585, DIVF=3840 → **32.6 kHz** instead of 24 MHz. V24 reconfigures CM_GP2 → SRC=PLLD(500 MHz), DIVI=20 → **24 MHz** before XSHUTDOWN. Confirmed: CM_GP2CTL=662, CM_GP2DIV=82773.

**V25 — Sensor MIPI confirmed via frame_count**

Added `imx708_read_frame_count()` (reg 0x0005). At t=500ms after stream_on: **frame_count=55** → sensor is transmitting ~110fps MIPI. Problem is definitively on the Unicam/D-PHY receive side.

**V26 — Remove Unicam reset, dump VPU state**

Removed U_CTRL=0x02/0x00 reset (hypothesis: destroys VPU D-PHY calibration). Pre-init dump shows VPU left **ANA=0x777** (not 0x43), CLK=0xCCC00002 (bit 1 set = calibration), CLT=0. CLK still frozen PRE=POST.

**V27 — Zero D-PHY writes (definitive test)**

Removed ALL writes to ANA, CLK, CLT, DAT0, DAT1. Only wrote IDI0/IPIPE/DMA/CTRL. Result: **CLK=0xCCC00002 STILL frozen**. Software register writes are definitively NOT the cause of D-PHY not receiving HS signals.

**V28 — Clock gate fix + CLE bit (current, awaiting flash)**

Two hypotheses:
1. Clock gate wrong address: code used 0x3F803000; Linux DT specifies 0x3F802004. V28 writes both.
2. CLE bit: VPU left CLK bit 1 (calibration) but not bit 0 (CLE = clock lane enable). V28 adds CLK|=0x01, DAT0/1|=0x01.

### Open issues — after V28

1. **If V28 unlocks D-PHY (CLK changes PRE→POST)**: check ISTA_FE, WP delta. If WP advances but ISTA=0, try IDI0=0x00 with longer wait. If WP < full frame, embedded lines are filling the buffer — adjust IBSA/IBEA.

2. **If V28 still frozen**: remaining hypotheses:
   - **VPU mailbox command required**: CSI port activation may need a specific GPU mailbox call (our mailbox currently only calls device power-on with device_id=12, which may be TRANSPOSER not IMAGE=6).
   - **Physical FFC issue**: MIPI signals may not be reaching BCM2837 CSI-1 pads (broken FFC, wrong orientation).
   - **Non-continuous clock**: sensor may use gated (non-continuous) clock mode; Unicam may need additional config.

3. **After first ISTA_FE**: validate debayer output matches expected sensor orientation, then integrate into main YOLO loop replacing `test_image.bin`.

---

## Potential Next Steps

| Idea | Effort | Expected gain | Notes |
|:---|:---:|:---:|:---|
| **Flash V28 + investigate D-PHY** | **Immediate** | **First full frame** | Test clock gate 0x3F802004 + CLE; if frozen: try VPU mailbox CSI activation or check FFC |
| Tune IDI0=0x2A after ISTA_FE | Low | Cleaner capture | Reject metadata/embedded CSI-2 packets once raw frames confirmed |
| conv1x1 8-ch | Low | ~10 ms | Same 8-ch pattern as conv3×3, applies to detection-head 1×1 layers |
| L0 stem (K=6, C_in=3) specialised unroll | Low | ~5–10 ms | C_in=3 allows full compile-time unroll of ci loop |
| PRFM prefetch on weight loads | Medium | 5–15% | Insert `prfm pldl1keep` 2–4 iterations ahead in weight inner loop |
| INT8 post-training quantisation | High | ~300 ms | 4× bandwidth reduction + 16 int8/register |
| V-Sync / double-buffer | Medium | — | Eliminate tearing; use GPU blanking interrupt |

Direct2Metal — YOLOv5n Bare-Metal on Raspberry Pi Zero 2 W
Hypothesis: A real-time object detection model runs significantly faster without an operating system, eliminating scheduler overhead, generic drivers, and high-level abstraction layers.

Approach: Port YOLOv5n (320×320 input, 80 COCO classes) to bare-metal AArch64 using ARM NEON SIMD, 4-core parallelism, and a custom MMU with D-cache, running on a Raspberry Pi Zero 2 W with no OS of any kind.

Current Result: 511 ms / ~2 FPS (real hardware)
Clase: 0 | Conf: 88 | Pos: [54,189]
Clase: 5 | Conf: 85 | Pos: [161,147]
Clase: 0 | Conf: 82 | Pos: [111,187]
Clase: 0 | Conf: 74 | Pos: [296,183]
>> Tiempo RGB:              4 ms
>> Tiempo L0 (K=6):        43 ms
>> Tiempo Backbone L1-L8: 272 ms
>> Tiempo Neck y Heads:   136 ms
>> Tiempo Decode + NMS:    22 ms
TOTAL TIME: 511 ms
Performance Summary
YOLOv5n 320×320 — Real Hardware Evolution
Milestone	Configuration	Time	FPS
Python / ONNX Runtime (Linux)	OS baseline	~1,300 ms	0.77
Bare-metal scalar C++ + D-cache	Single core	1,906 ms	0.52
4-core NEON, secondary cores uncached	Phase 6 (bug)	4,199 ms	0.24
4-core NEON, all cores cached	Phase 7 fix	598 ms	1.67
+ ISO 26262 safety + NMS fix + maxpool NEON	Phase 8	598 ms	1.67
+ 8-channel conv3×3 kernel	Current	511 ms	~2
The Phase 6 regression (4,199 ms > 1,906 ms scalar) happened because cores 1–3 ran without D-cache: every weight load hit uncached LPDDR2 (~10–50× slower), making Core 0 wait for three slow workers.

Per-bucket breakdown — current build vs 4-ch baseline
Bucket	4-ch baseline	8-ch current	Δ
RGB normalisation	5 ms	4 ms	−1 ms
L0 stem (K=6, C_out=16)	43 ms	43 ms	— (4-ch unchanged)
Backbone L1–L8	333 ms	272 ms	−61 ms (1.22×)
Neck + detection heads	161 ms	136 ms	−25 ms (1.18×)
Decode + NMS + UART + draw	23 ms	22 ms	−1 ms
Total	598 ms	511 ms	−87 ms
TinyYOLO 64×64 (Phase 3–5, historical)
Configuration	Total	FPS
Scalar C++ + D-cache	222 ms	4.5
NEON 4-core + D-cache	75 ms	13
Project Phases
Phase	Description	Key result
1	NEON GEMM 16×16 (assembly)	2.52× over scalar
2	NEON Conv2d (assembly)	1.75× over scalar
3	TinyYOLO 64×64 pipeline, QEMU validated	<1e-4 error vs PyTorch
4	MMU + D-cache (T0SZ=34, identity map 1 GB)	222 ms / 4.5 FPS
5	4-core parallel dispatch (task_epoch / done_count atomics)	75 ms / 13 FPS
6	YOLOv5n 320×320: NEON SiLU, atomic sync	4,199 ms — D-cache bug found
7	init_mmu() in secondary_main() — all cores fully cached	598 ms / 1.67 FPS
8	ISO 26262 safety hardening + NMS fix + NEON maxpool + 8-ch conv	511 ms / ~2 FPS
9	Pi Camera v3 (IMX708) bare-metal CSI-2 driver	V65: Unicam D-PHY mapped & terminated; debugging WP=0 latch
Architecture
┌──────────────────────────────────────────────────────────────┐
│  YOLOv5n Engine  (C++ / ARM NEON)    320×320 input           │
│  Backbone: Focus-like stem → C3 blocks → SPPF               │
│  Neck: PAN-FPN with upsample + concat                        │
│  Head: 3 detection scales (40×40, 20×20, 10×10)             │
├──────────────────────────────────────────────────────────────┤
│  Multicore Orchestrator  (src/multicore.cpp)                 │
│  Core 0: master   Cores 1-3: workers (all with D-cache)     │
│  Protocol: task_epoch (RELEASE) + done_count (ACQUIRE) + wfe │
│  QEMU fallback: 500K-cycle probe → single-core if no workers │
│  8-ch kernel selected when C_out ≥ 32 && C_out % 8 == 0     │
├──────────────────────────────────────────────────────────────┤
│  MMU + D-Cache + I-Cache  (src/mmu.cpp)                      │
│  Identity map 1 GB, T0SZ=34 (512 × 2 MB blocks)            │
│  RAM: Normal WB Inner Shareable  │  MMIO: Device-nGnRnE      │
│  All 4 cores independently enable their own MMU registers    │
├──────────────────────────────────────────────────────────────┤
│  BCM2837 — 4× Cortex-A53 @ 1.2 GHz — 512 MB LPDDR2         │
└──────────────────────────────────────────────────────────────┘
Phase 9 — Pi Camera v3 (IMX708) Bare-Metal CSI-2 Driver
Goal
Replace the static test_image.bin with live frames from the Pi Camera Module 3 (Sony IMX708, 12 MP). The camera outputs 1536×864 RAW8 RGGB via MIPI CSI-2 at 2 lanes, then debayered and center-cropped to 320×320 for the YOLO pipeline.

Key Discoveries (Hardware & Reverse Engineering)
Through extensive iterative hardware testing (Versions 1 through 65), several undocumented or poorly documented aspects of the BCM2835 Unicam block were decoded:

1. The Anatomy of WP = 0 (Write Pointer)
WP (register 0x11C) reading 0 is not always an error. When the DMA is enabled (MEM=1), the hardware deliberately holds the Write Pointer at 0 as a "waiting state". It only latches to the buffer base address (IBSA0) the exact millisecond the Protocol Engine (CPE) decodes a valid CSI-2 Frame Start (0xB8) packet.

2. D-PHY Termination Resistors (0x0F fix)
To correctly read the 450Mbps MIPI signal, the BCM2835 PHY requires specific bits set in U_DATn and U_CLK. Writing 0x1D (as seen in older examples) sets the power-down bit (LANE_PD = 1) and disables the termination resistor (TERM_EN = 0). The correct bitmask is 0x0F (LANE_EN=1, TERM_EN=1, LP_PWR=1, HS_PWR=1). Without the 100-ohm termination, high-speed signals bounce, creating electrical noise that the CPE interprets as garbage, preventing ISTA (interrupts) from firing.

3. BCM2837 True Register Map
Previous assumptions about the Crop Window registers were wrong and caused silent AXI Bus aborts:

0x110: Buffer 0 Start (IBSA0)

0x120: Image Window Width (IHWIN) — Previously mistakenly thought to be Buffer 1.

0x124: Image Window Height (IVWIN)

0x128: Buffer 1 Start (IBSA1)

Writing frame dimensions into 0x128 caused the DMA to detect an unconfigured ping-pong buffer, panicking the hardware. Also, IHWIN/IVWIN format is (Offset << 16) | Size, not the other way around.

4. The "Receiver First" Rule
The Unicam Protocol Engine (CPE=1) and DMA (MEM=1) must be armed before issuing the stream_on (0x0100=0x01) I2C command to the IMX708. Waking the camera first means the Unicam misses the initial LP-11 to HS sequence and the Frame Start packet, rendering the receiver deaf for the remainder of the stream.

Current status — V64/V65 Transition
Subsystem	Status	Notes
I2C (BSC1) & Registers	✅	~135 registers injected; IMX708 55fps MIPI confirmed via readback.
Unicam Register Map	✅	IHWIN/IVWIN formats corrected (0x120/0x124). Silent AXI panics eliminated.
Unpacker / IPIPE	✅	IPIPE=0x80 configured to allow memory packing to SDRAM.
Sequence Order	✅	Unicam CPE and MEM armed via CTRL=0x03 prior to imx708_stream_on().
Physical Layer (D-PHY)	⏳	V64 detected LP-11 (51712) but lacked 0x0F termination for HS lock. V65 applies the 100-Ohm fix.
Open issues & Next Steps
Test V65 (The PHY Lock): With REG[U_DATn/4] = 0x0F enabling the 100-Ohm termination on both lanes, the D-PHY should finally absorb the MIPI signal cleanly, allowing the CPE to read the 0xB8 Frame Start byte and latch the WP.

RAW8 vs RAW10 format mismatch: If V65 still yields ISTA=0, the IMX708 might be defaulting to RAW10 (0x2B). The Unicam IDI0 register is strictly filtering for RAW8 (0x2A). We will test IDI0=0x2B or use pure bypass (IPIPE=0x00).

Debayer Integration: Once ISTA_FE (Frame End) fires and WP reaches the buffer end, hook the memory address to camera_debayer.cpp and feed the 320x320 tensor into the YOLO engine.

Potential Next Steps
Idea	Effort	Expected gain	Notes
Flash V65 + investigate D-PHY Lock	Immediate	First full frame	Validate if 100-Ohm termination resolves the CSI-2 sync byte detection.
Tune IDI0 Data Type (RAW8 vs RAW10)	Low	Frame Decode	Change IDI0 to 0x2B if IMX708 is ignoring our RAW8 mode request.
conv1x1 8-ch	Low	~10 ms	Same 8-ch pattern as conv3×3, applies to detection-head 1×1 layers.
PRFM prefetch on weight loads	Medium	5–15%	Insert prfm pldl1keep 2–4 iterations ahead in weight inner loop.
INT8 post-training quantisation	High	~300 ms	4× bandwidth reduction + 16 int8/register.