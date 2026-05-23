# Direct2Metal — Goals

> Last updated: 2026-05-20
> Companion to PLAN.md (which tracks tactical version-by-version progress).
> This document tracks **strategic direction**: where the project is heading,
> what we explicitly will and will not pursue, and why.

---

## Mission

A complete vertical computer-vision stack — sensor to display — running on
a Raspberry Pi Zero 2 W with **no operating system**, no kernel, no
userspace libraries. Boot to first inference in milliseconds; deterministic
latency; total auditability of every layer from D-PHY to NMS.

---

## Long-term vision

What we are building is **not** "YOLO on a Pi". It is a deterministic,
auditable, OS-free baseboard support package + inference runtime where the
application (model, labels, post-processing, display layout) is
swappable without touching the BSP. The model and the dataset are
artifacts; the BSP/runtime is the product.

Success looks like: a third party can drop a new `app/<their_model>/`
folder, set `APP=their_model` in the Makefile, and ship a single
`kernel8.img` for a new vertical without opening any file under `bsp/` or
`runtime/`.

---

## Near-term goals (1–3 months)

### G1 — BSP / application boundary

Cleanly separate hardware abstraction, inference runtime, and application
logic. Today `kernel.cpp` mixes all three.

- [x] **Step 1**: consolidate loose `extern` declarations into `src/bsp.h`
      (no file moves, no API redesign). — *done 2026-05-20*
- [x] **Step 2**: added `bsp_fb_t` + `bsp_fb_get()` accessor (transitional —
      `lfb`/`pitch` still exposed in `bsp.h` until call sites are migrated);
      cleaned duplicate externs in `hud.cpp`, `camera.cpp`,
      `camera_debayer.cpp`. HW + sim builds clean, QEMU sim runs 3 frames
      with full YOLOv8n pipeline, clean exit, no regressions vs V170.
      — *done 2026-05-20*
- [x] **Step 3**: deleted dead `parallel_debayer_start/wait` + `TASK_DEBAYER`
      from multicore.h/cpp (no callers since V167 layout change); introduced
      `bsp_frame_acquire()` in bsp.h (wraps `unicam_capture_frame + ae_step`);
      declared `debayer_raw10_to_chw320` in bsp.h; migrated `kernel.cpp:342`
      from `camera_capture_frame()` to `bsp_frame_acquire() +
      debayer_raw10_to_chw320()`; removed now-dead `camera_capture_frame`
      and `camera_capture` from camera.h/cpp. HW + sim builds clean, QEMU
      sim runs 3 frames cleanly. *Exposed a latent V170 bug — see "Known
      issues" below.* — *done 2026-05-20*
- [x] **Step 4**: split `kernel.cpp` (663 L) into `kernel.cpp` (69 L,
      pure boot — UART, timer, mailbox, MMU, secondary-core spawn,
      `kernel_main`) and `yolo_v8n.cpp` (596 L — model + WeightStream +
      DFL decode + NMS + render + HUD update). Public surface declared
      in new `yolo_v8n.h` (a single `run_yolo_complete()`). Promoted UART
      + timer + `flush_to_ram` declarations to `bsp.h` so the app side
      pulls them through the BSP contract. HW + sim builds clean, QEMU
      sim runs 3 frames cleanly with no regression. File moves into
      `bsp/` and `app/yolo_v8n_coco/` deferred to Step 5. — *done 2026-05-20*
- [x] **Step 5**: `src/` retired. Files moved with `git mv` (history
      preserved) into `bsp/` (boot + framebuffer + MMU + multicore +
      camera/sensor + watchdog + mailbox + sdcard + safety_config),
      `runtime/` (ops.{h,cpp}, conv2d.cpp) + `runtime/neon/` (matmul +
      conv2d NEON kernels), and `app/yolo_v8n_coco/` (yolo_v8n.{h,cpp},
      hud, data.s, weights blobs, test_image, COCO labels). Makefile
      gained `APP ?= yolo_v8n_coco` and uses VPATH so object files stay
      flat at the build root (filenames are globally unique). Include
      paths become `-Ibsp -Iruntime -Iapp/$(APP)`. Linker script moved
      to `bsp/linker.ld`. HW + sim builds clean, QEMU sim runs 3 frames
      with full YOLOv8n pipeline, clean exit, no regressions vs Step 4.
      — *done 2026-05-20*

**G1 complete.** Adding a second application now lives at
`app/<other>/{yolo_v8n.h-equivalent, model.cpp, hud.cpp, weights.bin}`,
selected with `make APP=<other>`. No BSP changes required.

The architectural rationale is in the conversation that spawned this file;
condensed: the same person who would write `app/license_plates/` today must
edit five files in three layers (`kernel.cpp`, `hud.cpp`, `camera.h`,
`multicore.h`, `model_config.h`). After G1 it should be one folder.

### G2 — Throughput viability

Reach a frame rate where real-world applications are worth pursuing.
Today: **1.41 fps** at YOLOv8n 256² FP32 (V170). Track B in PLAN.md
projects ~13–15 fps after INT8 + frame-skip.

- **B3 — INT8 quantization** (`tools/B3_INT8_PLAN.md`). Calibrated
  offline on ~50 real frames, `vmlal_s16` + `vmovl_s8` kernels.
  Three tiers (gain × effort):
  - **Tier 1 (W8A32, dequant on load) — ×1.2-1.5, ~2-3 days.**
    - [x] *Slice 1 (2026-05-20)*: data.s wires `weights_int8.bin`
          (3.17 MB) alongside the FP32 blob; runtime CRC check
          (`WEIGHTS_INT8_CRC32`) validates the blob shipped intact;
          `WeightStreamINT8` reader + `dequant_int8_to_fp32_neon` NEON
          helper added and compile-tested. Image grew 13.9 → 17.0 MB.
          FP32 path byte-identical, INT8 path unused yet.
    - [x] *Slice 2 + 3 (2026-05-22)*: `parallel_conv2d_int8` /
          `parallel_conv1x1_int8` wrappers (dequant into a shared 350 K-
          float scratch then call the existing FP32 kernel); unified
          `LayerHandle` + `LAYER_LOAD/CONV2D/CONV1X1/WS_INIT/WS_TYPE`
          macros that specialise FP32 vs INT8 at compile time on
          `-DUSE_INT8_WEIGHTS`; `c2f_real_inference` and
          `sppf_real_inference` switched to `WS_TYPE&` + macros;
          Makefile gained `USE_INT8 ?= 0`. Both flavours build clean;
          QEMU sim INT8 runs end-to-end (1414 ms/frame vs 995 ms FP32,
          **+42 % regression**) and emits plausible detections under
          synthetic input. Correctness path **validated**; performance
          path **not yet** — see below.
    - [x] *HW measurement (2026-05-22)*: bench on Pi Zero 2 W with
          `tools/bench_int8_ab.py` over 3 steady-state frames each.
          FP32 = 725 ms total (268 bb / 145 neck / 268 head); INT8 = 705
          ms (261 / 141 / 257). **INT8 -2.8% net, all conv stages
          shave 2-4%.** Detections preserved: same class (person),
          confidence -2 to -3 pp (83% → 80-83%). The L1/L2 cache
          benefit on A53 is real but modest at this model size; the
          FMA throughput ceiling dominates as the plan predicted.
    - **Tier 1 stopped at Slice 3.** Marginal positive return (×1.028
      vs the optimistic ×1.2-1.5 in the plan). Slice 4 (dequant fused
      in `conv2d_partial_8ch` / `conv1x1` NEON inner loop) would add
      another ~5-10 pp but the effort doesn't justify it when Tier 2
      promises ×2-3 and reuses none of Tier 1's runtime kernels.
      `USE_INT8=1` build retained as alternative — runs, is faster, has
      comparable detection quality.

  - **QEMU lesson learned**: QEMU `raspi3b` has no cache model. It
    predicted Tier 1 would be +42% *slower*; real A53 was -2.8%
    *faster*. Any bandwidth-sensitive optimization must be benched on
    silicon, not QEMU.
  - **Tier 2 (W8A8, full integer) — ×2-3, 1-2 weeks.** Only after
    Tier 1 is the bottleneck.
  - **Tier 3 (A53-specific layouts) — ×3-4, ≥2 weeks.** Defer
    indefinitely; not justified yet.
- [ ] **B4 — Frame-skip detection**: infer every N frames, hold bboxes,
      keep the camera + HUD at 52 fps perceived.
- [ ] Stop point: ~15 fps real / ~25 fps perceived. Beyond that the A53
      bare-metal target tops out (no SDOT, FP32 NEON 4-wide). Faster
      requires Pi 4/5 (A72/A76) or a smaller model.

### G3 — Reproducibility and toolchain stability

- [ ] CI sanity build (Docker-based, current `rpi-forge` image) on every
      commit. Even just `make kernel.o` per file catches drift.
- [ ] CRC-verified weights blob at boot (`weights_int8_crc.h` already
      seeded — wire the runtime check).
- [ ] One README-tested cold-start path: `git clone` → `make` →
      QEMU run → `./flash.sh` on an SD card.

---

## Strategic principles

These are the rules of engagement. When in doubt, fall back to these.

1. **OS-free is a feature, not a constraint.** Determinism, boot time,
   and attack surface come from this. Any feature that "would be easier
   with a kernel" needs strong justification to live in this repo.
2. **Application code never touches MMIO.** If app code names a peripheral
   register, the BSP boundary has leaked. Fix the boundary.
3. **No virtual / vtable abstractions until we have ≥2 real
   implementations.** No "AbstractSensor" until there is a second sensor.
4. **Reproducibility beats convenience.** Same input → same output, every
   time. No randomness in inference or rendering paths.
5. **The Makefile is the spec.** It must show in plain text what compiles
   into what target. No CMake / Bazel / build-system tax.
6. **A bug is a transport bug only when proven so.** The V140–V147
   "lane swap" saga is the cautionary tale: spent weeks chasing transport
   for what turned out to be an IMX708 test-pattern × binning interaction.
   Default suspect is the most recent change, not the hardware.

---

## Target verticals (where our advantages are decisive)

We will not chase generic robotics or smart-home use cases — Linux + ROS
crushes us on ecosystem there. Where bare-metal wins:

- **Fixed industrial inspection** — one camera, one task, deterministic
  latency, no network. Our boot time and latency floor are real
  differentiators here.
- **Air-gapped / offline forensics** — wildlife cameras, security where
  network connectivity is itself the threat model. Our zero-CVE-inheritance
  surface matters.
- **Educational platform** — the codebase is legible end-to-end. Very few
  projects let a student trace a pixel from D-PHY to bounding box in one
  repo. Potential kit / curriculum product.
- **Safety-relevant subsystems** — `safety_config.h` and the
  ISO 26262-style epoch protocol in `multicore.cpp` already lean this way.
  Worth keeping the discipline; do not let it rot.

---

## Explicit non-goals

These are tempting and will be politely declined:

- **No network stack.** No Ethernet, no Wi-Fi, no TCP/IP, no MQTT. If
  remote telemetry is needed, the answer is UART → external bridge.
- **No filesystem beyond `sdcard.cpp`'s raw block read.** No FAT, no ext.
- **No dynamic memory allocator.** All buffers are static. The 12 MB of
  RAM in BSS is the budget; if it doesn't fit, change the model.
- **No competing with Coral / Jetson on raw fps.** Our axis is
  determinism + footprint + auditability, not throughput.
- **No porting to other SoCs** until G1 (BSP boundary) is done and at
  least one second application exists. Premature portability is wasted
  work.
- **No CMake / Bazel** unless the current Makefile becomes unmaintainable
  (it has 80 lines — that day is far off).

---

## Known issues (surfaced but not yet fixed)

- ~~**`cam_frame` undersized — debayer overflow on every capture.**~~
  ***Retracted 2026-05-20.*** Closer inspection showed that V170 also
  updated `OUT_W/OUT_H` in `camera_debayer.cpp` to 256, so the debayer
  output (3 × 256 × 256) matches `cam_frame` exactly. The only real
  overflow was in the capture-failure path's zero-fill loop in
  `camera_capture_frame` (wrote 3·320·320 instead of 3·256·256) — a
  442 KB overflow on the failure path only. Step 3 already closed that
  case when the migration replaced the loop with `3 * YOLO_IN * YOLO_IN`.
  *Step 3.5 (2026-05-20)*: renamed `debayer_raw10_to_chw320` →
  `debayer_raw10_to_chw_yolo`, moved `YOLO_IN` to `bsp.h` so the debayer
  and the model can't drift again, derived `OUT_W/OUT_H/STEP_Q8` from
  `YOLO_IN` instead of hard-coding them.

- **`src/test_image.bin` is sized for `YOLO_IN=320`** (1 228 800 bytes
  vs the 786 432 bytes the `YOLO_IN=256` path expects). When
  `g_use_camera == false` (QEMU/test path), YOLO reads
  `test_image[0..196607]` interpreting it as `3 × 256² CHW`, but the
  binary's actual layout is `3 × 320² CHW`. No UB (read stays inside
  the 1.2 MB buffer) but the channels are mis-bisected — YOLO's "G
  plane" is actually the tail of the binary's R plane plus the start
  of the binary's G plane, and YOLO's "B plane" is mostly the binary's
  G data. Only matters for the no-camera fallback; hardware path is
  fine. Fix: regenerate `test_image.bin` from a 256×256 source via
  `tools/`. Low priority.

## How this document is used

- New strategic decisions get a one-line entry under "Strategic principles"
  or "Explicit non-goals".
- New milestones go under "Near-term goals" with a checkbox.
- Tactical / per-version progress stays in **PLAN.md** — do not duplicate.
- When a near-term goal completes, it stays in this file (struck through
  or dated) so the trajectory is visible.
