# Direct2Metal — YOLOv5n Bare-Metal on Raspberry Pi Zero 2 W

**Hypothesis:** A real-time object detection model runs significantly faster without an operating system, eliminating scheduler overhead, generic drivers, and high-level abstraction layers.

**Approach:** Port YOLOv5n (320×320 input, 80 COCO classes) to bare-metal AArch64 using ARM NEON SIMD, 4-core parallelism, and a custom MMU with D-cache, running on a Raspberry Pi Zero 2 W with no OS of any kind.

---

## Performance Summary

### TinyYOLO 64×64 (validated end-to-end, Phase 3–5)

| Configuration | Total | FPS | Timer |
| :--- | :---: | :---: | :---: |
| Scalar C++ (no cache) | ~6,900 ms | 0.14 | 19.2 MHz |
| Scalar C++ + D-cache | 222 ms | 4.5 | 19.2 MHz |
| NEON 4-ch single-core + cache | ~129 ms | ~7.7 | 19.2 MHz |
| **NEON 4-core + cache** | **75 ms** | **13** | **19.2 MHz** |

Per-layer breakdown (4-core NEON, real hardware):

| Layer | Scalar+cache | 4-core NEON | Speedup |
|---|:---:|:---:|:---:|
| L1 Conv 3→16 | 29 ms | 12 ms | 2.4× |
| L2 Conv 16→32 | 95 ms | 29 ms | 3.3× |
| L3 Conv 32→64 | 92 ms | 28 ms | 3.3× |
| **Total** | **222 ms** | **75 ms** | **~3×** |

> Effective speedup = NEON 1.72× × multicore 1.72× ≈ 2.96×. Parallel efficiency ~75% (bottleneck: shared L2 cache between all 4 cores).

### YOLOv5n 320×320 (Phase 6–7, in progress)

| Configuration | Total | FPS | Status |
| :--- | :---: | :---: | :---: |
| Baseline (ONNX Runtime, Python, RPi OS) | ~1,300 ms | 0.77 | Measured |
| Bare-metal scalar + D-cache | ~1,906 ms | 0.52 | Measured |
| NEON 4-core, secondary cores **without** D-cache (Phase 6 bug) | ~4,199 ms | 0.24 | Measured — regression |
| **NEON 4-core, all cores cached (Phase 7 fix)** | **~200–500 ms** | **~3–5** | Expected |

> The Phase 6 regression (4,199 ms > 1,906 ms scalar) happened because cores 1–3 ran without D-cache: every load from the 7.5 MB weight tensor hit uncached LPDDR2 (~10–50× slower), making Core 0 wait for three slow workers instead of one fast one.

### C3 block evolution (YOLOv5n internal, 160×160 spatial map)

| Version | Configuration | Latency | Speedup |
|:---:|---|:---:|:---:|
| V1 | 1 core, scalar C++ | 2,835 ms | baseline |
| V2 | 4 cores, scalar C++ | 1,333 ms | 2.1× |
| V3 | 4 cores, NEON intrinsics | 656 ms | 4.3× |

---

## Project Phases

| Phase | Description | Key result |
|:---:|---|---|
| 1 | NEON GEMM 16×16 (assembly) | 2.52× over scalar |
| 2 | NEON Conv2d (assembly) | 1.75× over scalar |
| 3 | TinyYOLO 64×64 inference pipeline, QEMU validated | <1e-4 error vs PyTorch |
| 4 | MMU + D-cache (T0SZ=34, identity map 1 GB) | 222 ms / 4.5 FPS on HW |
| 5 | 4-core parallel dispatch (task_epoch/done_count atomics) | 75 ms / 13 FPS on HW |
| 6 | YOLOv5n 320×320: L0 K=6 NEON, NEON SiLU, atomic sync, no UART hot path | 4,199 ms — D-cache bug found |
| **7** | **Per-core D-cache enable in `secondary_main`** | **Expected ~200–500 ms on HW** |

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
├──────────────────────────────────────────────────────────────┤
│  MMU + D-Cache + I-Cache  (src/mmu.cpp)                      │
│  Identity map 1 GB, T0SZ=34 (512 × 2 MB blocks)            │
│  RAM: Normal WB Inner Shareable  │  MMIO: Device-nGnRnE      │
│  All 4 cores independently enable their own MMU registers    │
├──────────────────────────────────────────────────────────────┤
│  BCM2837 — 4× Cortex-A53 @ 1 GHz — 512 MB LPDDR2           │
└──────────────────────────────────────────────────────────────┘
```

### Software stack

```
[PyTorch on host PC]
    │  export_model.py → repack weights to [C_out/4][C_in×K×K][4]
    ↓
[weights.bin + test_image.bin] ──incbin──→ [.rodata in kernel8.img]
    ↓
[start.s] → EL2→EL1 (all cores), CPACR.FPEN=0b11, BSS clear, → kernel_main()
    ↓
[mmu.cpp::init_mmu()] → 1GB identity table, T0SZ=34, MMU+D$+I$ ON (all 4 cores)
    ↓
[kernel.cpp::run_yolo_complete()]
    ├── parallel_conv2d / parallel_conv1x1  →  [multicore.cpp: 4-core dispatch]
    │       └── conv2d_partial()            →  [NEON: vld1q_f32 + vmlaq_n_f32]
    ├── silu_inplace()                      →  [ops.cpp: neon_expf4 + Newton-Raphson]
    ├── upsample2x_nearest / maxpool5x5_s1_p2
    ├── decode_yolo_grid() × 3 scales       →  sigmoid, anchor decode
    └── NMS (bubble sort + IoU 0.45)
    ↓
[UART] → "TOTAL TIME: XXX ms"    [HDMI framebuffer] → image + bounding boxes
```

---

## Project Structure

```
src/
├── start.s          — Boot entry: EL2→EL1, NEON enable, bss_ready, per-core stacks
├── kernel.cpp       — Main loop: YOLOv5n layers, NMS, HDMI rendering
├── multicore.cpp    — 4-core dispatch engine (parallel_conv2d / parallel_conv1x1)
├── multicore.h      — ParallelTask struct, sync state, public API
├── ops.cpp          — NEON kernels: conv2d_neon_4ch, conv1x1, SiLU, upsample, maxpool
├── ops.h            — Operator declarations
├── mmu.cpp          — MMU + D-cache init (all 4 cores via mmu_table_ready flag)
├── mmu.h
├── mailbox.cpp      — GPU mailbox: framebuffer allocation, cache-coherent MMIO
├── video.cpp        — Framebuffer draw primitives
├── conv2d.cpp       — Dispatch wrapper (NEON or scalar fallback)
├── conv2d_neon.s    — Legacy NEON conv2d (assembly, Phase 2)
├── data.s           — .incbin of weights.bin + test_image.bin → .rodata
├── weights.bin      — YOLOv5n weights in NEON-repacked format (7.5 MB)
├── test_image.bin   — 320×320 test image, CHW float32 (1.2 MB)
└── linker.ld        — Loads at 0x80000; places .rodata, .data, .bss

tools/
├── export_model.py  — Exports YOLOv5n from PyTorch → weights.bin
└── convert_image.py — Converts arbitrary image to test_image.bin format

build/
└── config.txt       — RPi firmware config (arm_64bit, UART, HDMI, gpu_mem)
```

---

## Build

### Prerequisites

- Docker image `rpi-forge` (contains `aarch64-linux-gnu-g++` cross-compiler)
- Python 3 + PyTorch (to regenerate weights)

### Steps

```bash
# 1. Export weights (required after any change to export_model.py)
python3 tools/export_model.py

# 2. Build the bare-metal kernel
docker run --rm -v $(pwd):/app rpi-forge make kernel8.img

# 3. Smoke-test in QEMU (single-core fallback — correct for QEMU raspi3b)
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none
```

Expected QEMU output:
```
=== Direct2Metal: MOTOR IA EN TIEMPO REAL ===
...
[CHK 16] Entrando a NMS... Predicciones crudas: 39
[CHK 17] NMS Terminado. Dibujando...
>>> OBJETOS DETECTADOS <<<
...
TOTAL TIME: ~4200 ms   ← single-core fallback, normal for QEMU
```

QEMU always shows single-core timing because `raspi3b` emulation does not start secondary cores. On real hardware with all 4 cores cached, the expected time is ~200–500 ms.

---

## Flashing to Hardware

| File | Source | Notes |
|---|---|---|
| `bootcode.bin`, `start.elf`, `fixup.dat` | RPi firmware repo | Must already be on the SD card |
| `config.txt` | `build/config.txt` in this repo | Sets 64-bit mode, UART, HDMI |
| `kernel8.img` | Built by `make kernel8.img` | Our bare-metal kernel |

If `config.txt` is already correct on the SD card, copying only `kernel8.img` is enough:

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
gpu_mem=64            # Reserve 64 MB for GPU/framebuffer
hdmi_force_hotplug=1  # Force HDMI even without a monitor detected
```

### Serial debug

Connect a USB-UART adapter to **GPIO14 (TX)** / **GPIO15 (RX)** at **115200 baud 8N1**. Every inference prints:

```
TOTAL TIME: XXX ms
```

---

## Synchronisation Protocol (multicore)

```
Core 0                          Cores 1-3 (secondary_main)
───────────────────────────     ──────────────────────────────────────
[start.s: EL2→EL1, NEON]       [start.s: EL2→EL1, NEON, per-core stack]
kernel_main():                  secondary_main():
  bss_ready = 1 + sev ───────►    init_mmu() → wait mmu_table_ready
  video_init()                                → enable own MMU+cache
  init_mmu():                     __atomic_add(cores_ready) ← now = 3
    fill table                    loop:
    mmu_table_ready=1+sev ─────►    ACQUIRE-poll task_epoch
    enable own MMU+cache            read task snapshot
  while(1):                         execute assigned slice
    run_yolo_complete()             RELEASE-add done_count
      parallel_conv2d():            sev
        probe (500K nop) ──────►  cores_ready==3 ✓ → cached workers
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

## Detection Configuration

| Parameter | Value | Notes |
|---|---|---|
| Input resolution | 320×320 | CHW float32, normalised [0,1] |
| Confidence threshold | 0.5 | Score = obj_conf × max_class_prob |
| NMS IoU threshold | 0.45 | Same-class suppression only |
| Max predictions (pre-NMS) | 100 | Hard cap; raise if needed |
| Anchors P3 (stride 8, grid 40×40) | [10,13], [16,30], [33,23] | Standard YOLOv5n, pixel units |
| Anchors P4 (stride 16, grid 20×20) | [30,61], [62,45], [59,119] | |
| Anchors P5 (stride 32, grid 10×10) | [116,90], [156,198], [373,326] | |
| COCO classes | 80 | Class 0 = person |

> **Anchor note**: `anchor_grid = anchors × stride` is always in input-image pixel space, regardless of whether the model was trained at 640×640 or run at 320×320. Do **not** rescale anchors by resolution ratio.

---

## HDMI / Video Stability Notes

Running all 4 Cortex-A53 cores at 100% saturates the BCM2837 shared memory bus. The VideoCore IV GPU competes for the same bus to fetch the framebuffer for HDMI output, causing "No Signal" if the GPU's DMA requests are starved.

**Mitigations applied:**
- `dc civac` flush + `0xC0000000` bus alias in `mailbox.cpp` to ensure GPU-visible framebuffer writes
- `flush_to_ram()` called in `video_flush()` after every frame draw
- `hdmi_force_hotplug=1` in `config.txt`

**Known remaining issue:** at full 4-core load, the display may lose sync briefly on slower monitors. Adding `force_turbo=1` to `config.txt` locks the memory controller clock and eliminates this. Not set by default to avoid thermal throttling on passive cooling.

**V-Sync (not yet implemented):** Drawing directly to the framebuffer without syncing to the GPU's blanking period causes tearing on fast screens. A proper implementation would use the Mailbox VSYNC interrupt and double-buffer.

---

## Potential Improvements

Ordered by expected impact-to-effort ratio.

### 1. Per-layer profiling
Add `get_timer_count()` around each layer call to identify which layers dominate. Without this data, further optimisation is guesswork.

### 2. INT8 quantisation
Cortex-A53 NEON processes 16 × INT8 per instruction vs 4 × FP32 → theoretical 4× throughput gain. YOLOv5n INT8 loses <1% mAP. This single change could bring 300 ms → ~75 ms if kernels are clean.

### 3. Cache-aware tiling
L0 output (16×160×160 = 1.6 MB), L5, and C3 blocks exceed the 512 KB shared L2. Tiling spatial dimensions (e.g., 32×32) keeps working sets in L1 and avoids repeated DRAM fills for the same weight rows.

### 4. Winograd F(2×2, 3×3)
Reduces a 3×3 conv from 36 to 16 MACs per 2×2 output tile (2.25× reduction). Used by TFLite and ONNX Runtime Mobile for exactly this kernel.

### 5. PRFM prefetch on weight loads
Insert `asm volatile("prfm pldl1keep, [%0, #128]" :: "r"(ptr))` 2–4 iterations ahead in the weight inner loop to hide DRAM latency on the 7.5 MB weight tensor.

### 6. NEON residual add and concat
C3 block shortcut adds and PAN neck `concat_tensor` calls are scalar C++ loops over up to 204,800 floats. Converting to `vld1q_f32` / `vaddq_f32` / `vst1q_f32` gives ~4× on those steps.

### 7. Real-time camera input
The kernel processes a static test image embedded in `.rodata`. Connecting a Raspberry Pi Camera v2 via CSI and reading frames through the Unicam peripheral registers would enable real detection.

### 8. Frame pipeline overlap
While Core 0 runs NMS + draw + UART for frame N, cores 1–3 could begin convolution for frame N+1. Only worthwhile if post-processing exceeds ~50 ms.

### 9. INT8 / O(n) NMS
Current NMS is O(n²) bubble sort. A spatial hash (one bin per anchor cell) gives O(n) and removes the hard `MAX_PREDS=100` cap.

### 10. Depth-wise separable L0 stem
L0 is a standard Conv(3→16, K=6, stride=2) on a 320×320 input — the costliest layer at that resolution. Replacing it with DW-separable (K=6 DW + 1×1 PW) cuts FLOPs by ~9×.

---

## Last Modified Files

Files changed since the last git commit (`da3e746`), covering Phases 6 and 7:

| File | Change |
|---|---|
| `src/multicore.cpp` | **Phase 7**: added `init_mmu()` call in `secondary_main()` before `cores_ready` increment; L0 K=6 NEON dispatch (condition changed to `C_out%4==0`); `flush_to_ram` replaced with `dmb ish` for CPU–CPU sync; `parallel_conv1x1` added |
| `src/multicore.h` | Unified `ParallelTask` struct with `TASK_CONV2D` and `TASK_CONV1X1` discriminant; extern declarations for `bss_ready`, `cores_ready`, `task_epoch`, `done_count` |
| `src/start.s` | Restored complete version: EL2→EL1 transition for all cores, `CPACR_EL1.FPEN=0b11`, `bss_ready` in `.data`, per-core stacks (0x70000 / 0x60000 / 0x50000) |
| `src/ops.cpp` | NEON vectorised SiLU: `neon_expf4` helper (bit-manipulation 2^k + polynomial) + `vrecpeq_f32` + 2× Newton-Raphson reciprocal for sigmoid |
| `src/ops.h` | Added `silu_inplace`, `maxpool5x5_s1_p2`, `upsample2x_nearest`, `ops_neon_conv1x1_kernel` declarations |
| `src/kernel.cpp` | YOLOv5n 320×320 full inference graph (backbone → neck → 3 detection heads); `bss_ready` signal before `video_init()`; confidence threshold raised to 0.5; anchors restored to standard YOLOv5n values |
| `src/mmu.cpp` | `mmu_table_ready` flag + secondary-core sync branch (was present but never triggered — now called from `secondary_main`) |
| `src/model_config.h` | Auto-generated: YOLOv5n layer dimensions and weight stream offsets |
| `src/weights.bin` | Regenerated: all layers with `C_out%4==0` (including L0 K=6) now in NEON-repacked `[C_out/4][C_in×K×K][4]` format |
| `src/test_image.bin` | Regenerated: 320×320 CHW float32 test image (was 64×64) |
| `tools/export_model.py` | Repack condition changed from `kernel_size==(3,3)` to `C_out%4==0`; loads YOLOv5n via `torch.hub`; prints detected anchor values |
| `Makefile` | Added `mailbox.o`, `video.o`, `mmu.o`, `multicore.o`; `-mno-outline-atomics` to avoid `__aarch64_ldadd4_rel` in `-nostdlib` |

---

## Key Bugs Solved

| Bug | Symptom | Root cause | Fix |
|---|---|---|---|
| Silent hang after `init_mmu()` | No UART output | `T0SZ=25` maps MMIO as Normal WB cached; UART writes go to L1, never reach hardware | Set `T0SZ=34` → Level-2 walk, MMIO mapped as Device-nGnRnE |
| WATCHDOG fires forever in QEMU | Secondary cores never acknowledge | `CPACR_EL1.FPEN` not set; no EL2→EL1 transition in `start.s` for secondary cores | Restored full per-core EL setup + NEON enable in `start.s` |
| L0 (K=6) always ran single-core scalar | High latency on stem layer | Dispatch condition was `K==3 && C_out%4==0` | Changed to `C_out%4==0`; `conv2d_partial` already supported arbitrary K |
| GPU mailbox sees zeros on real HW | Blank display | D-cache traps CPU writes in L1/L2; GPU DMA reads stale DRAM | `dc civac` flush + `0xC0000000` bus alias in `mailbox.cpp` |
| **Secondary cores slower than single-core** | **4,199 ms > 1,906 ms scalar baseline** | **`init_mmu()` never called from `secondary_main()`: cores 1–3 run without D-cache, hitting uncached LPDDR2 on every weight tensor load (~10–50× slower)** | **Call `init_mmu()` at the top of `secondary_main()` before incrementing `cores_ready`** |
| Bounding boxes drawn as lines | Visually thin/invisible boxes on HDMI | Anchors incorrectly halved (×0.5) — `anchor_grid = anchors × stride` is always in input-image pixel units regardless of input resolution | Reverted to standard YOLOv5n values: P3=[10,13], P5=[116,90], etc. |
| Too many false-positive detections | 100 overlapping boxes saturating display | Confidence threshold was 0.25; `MAX_PREDS=100` cap fills with low-quality boxes that NMS cannot suppress (different classes don't overlap) | Raised decode + draw threshold to 0.5 |

---

## Historical Benchmarks

All cycles measured on real RPi Zero 2 W hardware (ARM Generic Timer, 19.2 MHz) except QEMU rows.

| ID | Test | Configuration | Result |
|:---:|---|---|:---:|
| B01 | GEMM 16×16 | Scalar C++ -O3 | 16,106 cycles |
| B02 | GEMM 16×16 | NEON assembly | **6,383 cycles (2.52×)** |
| AI-01 | Conv2d 16×16 | Scalar C++ | 91,135 cycles |
| AI-02 | Conv2d 16×16 | NEON (copy-then-compute) | 186,944 cycles (FAIL — bandwidth overhead) |
| AI-03 | Conv2d 16×16 | NEON direct access | **51,933 cycles (1.75×)** |
| NN-01 | TinyYOLO 64×64 | Scalar C++ + D-cache | 222 ms / 4.5 FPS |
| NN-02 | TinyYOLO 64×64 | NEON 4-ch + D-cache | ~129 ms / ~7.7 FPS (est.) |
| NN-03 | TinyYOLO 64×64 | 4-core NEON + D-cache | **75 ms / 13 FPS** |
| YOLOv5-01 | YOLOv5n 320×320 | Scalar C++ + D-cache | 1,906 ms / 0.52 FPS |
| YOLOv5-02 | YOLOv5n 320×320 | 4-core NEON, secondary cores **uncached** | 4,199 ms / 0.24 FPS (bug) |
| YOLOv5-03 | YOLOv5n 320×320 | 4-core NEON, **all cores cached** (Phase 7) | pending measurement |
