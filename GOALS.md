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
  - **Tier 2 (W8A8, full integer) — ×2-3, 1-2 weeks.**
    - [x] *Session 1 (2026-05-22)*: `tools/calibrate_int8.py` captures
          per-tensor symmetric activation scales by running ~50 coco128
          images through YOLOv8n FP32 with forward hooks on each Conv
          module's wrapper (post-SiLU). Output:
          `app/yolo_v8n_coco/weights_int8_w8a8.bin` (3.17 MB) with
          per-layer (n_w, scale_w, scale_in, scale_out, int8 weights,
          int32 bias) + CRC header. Bias pre-multiplied by
          `1/(scale_in * scale_w)` so it's addable to the int32
          accumulator with no float ops in the hot loop. 63 layers
          calibrated; sat_w ≈ 0 % (weights not clipped). Smoke tested
          with 20 images; production run should use ≥ 50.
    - [x] *Session 2 (2026-05-22)*: NEON int8 conv kernels in new
          `runtime/ops_int8.cpp`. Three single-core entry points:
          `conv1x1_int8` (1×1 flat weights, vectorized over 8 input
          positions per output channel), `conv2d_neon_4ch_int8` (K×K
          with 4 co's per group), `conv2d_neon_8ch_int8` (K×K with 8
          co's per group). All use the `vld1_s8 → vmovl_s8 → vmlal_s16`
          A53 path (no SDOT on ARMv8.0). Out-stage `int32 → fp32 → SiLU
          → /scale_out → saturate`. Bias is int32, pre-added directly
          to the int32 accumulator. Build clean, kernels exported in
          object file (85 KB), kernel8.img links the new symbols, QEMU
          smoke test of the FP32 default path shows no regression.
    - [~] *Session 3 — Phase A (2026-05-23)*: scaffolding without
          model integration. data.s wires `weights_int8_w8a8.bin` (3.17
          MB) alongside the FP32 and W8A32 blobs. `WeightStreamW8A8`
          reader (per-layer `n_w, scale_w, scale_in, scale_out, int8
          weights, n_b, int32 bias`). `w8a8_conv2d_dispatch` picks
          4ch/8ch/1x1 kernel based on K and C_out. Int8 helpers added:
          `copy_tensor_i8`, `concat_tensor_i8`, `upsample2x_nearest_i8`,
          `maxpool5x5_s1_p2_i8`, `w8a8_residual_add` (fp32-domain with
          per-tensor scale ratios — int8 saturated add isn't correct
          when scales differ). Runtime CRC check for the W8A8 blob.
          Makefile gained `USE_INT8_W8A8=1` flag (mutually exclusive
          with `USE_INT8=1`; W8A8 wins). All 3 modes compile; FP32
          default + USE_INT8 builds clean, USE_INT8_W8A8 currently
          runs the FP32 path (no graph switch yet — Phase B).
    - [x] *Session 3 — Phase B (2026-05-23)*: model graph wired end
          to end. Added `act_t` typedef (int8_t in W8A8, float
          elsewhere), changed every inter-layer buffer
          (buf_A/B/scratch/save_*/head_*) to `act_t`; cam_frame stays
          fp32 since the debayer writes fp32. Extracted FP32 residual
          add into `fp32_residual_add_neon` helper. Macro block split
          into 3 branches (USE_INT8_W8A8 / USE_INT8_WEIGHTS / FP32),
          each defining WS_TYPE, WS_INIT, LAYER_LOAD, CONV2D, CONV1X1,
          COPY_TENSOR, CONCAT_TENSOR, UPSAMPLE2X, MAXPOOL5X5,
          RESIDUAL_ADD, DECODE_HEAD. `c2f_real_inference` tracks
          `L_prev_out` across bottleneck iterations so the W8A8
          residual gets correct `(x_out_scale, x_in_scale)` arguments.
          `run_yolo_complete` gained conditional input preprocess
          (fp32 → int8 via 127× scale for [0,1] inputs) and uses
          `DECODE_HEAD` macro which in W8A8 dequants int8 head outputs
          into scratch reinterpreted as fp32. QEMU sim: W8A8 runs 3
          frames cleanly end-to-end with both CRC checks passing
          (cap=11 l0=54 bb=685 neck=374 head=703 T=1815 ms vs FP32
          T=989 ms — single-core int8 vs 4-core fp32 explains the
          slowdown; HW will look different).
    - [x] *Session 4 — HW bench + diagnosis (2026-05-26)*.
          V177 W8A8 on Pi Zero 2 W: **2944 ms total vs FP32 727 ms
          (×4.05 slower)**. Per-stage ratio ~×4 exact — single-core
          int8 vs 4-core FP32, with no measurable L1/L2 cache win.
          **Detections: 0/13 frames vs FP32 4/18.**
          V178 added per-layer absmax instrumentation
          (`W8A8_DEBUG=1`) — revealed L6/L7/L8 saturating at i8=127
          (HW activations > coco128 calibration absmax in mid-backbone),
          while L1/L2/L21/P5_CLS underuse 36-44 % of int8 range. Tried
          `--scale_safety_margin 1.5`: L7 still saturated, P5_CLS
          worsened to 24 % range, still zero detections. A global
          scale bump can't fix heterogeneous per-layer divergence.
    - **Tier 2 closed (2026-05-26). Verdict: doesn't beat FP32 on
      A53.** Two structural reasons:
        1. *Performance ceiling = FP32*. A53 (ARMv8.0, no SDOT) needs
           the `vld1_s8 → vmovl_s8 → vmlal_s16` long path. Even with
           multicore int8 (Session 5 hypothetical) the per-conv
           overhead matches multicore FP32 — no win.
        2. *Correctness fragile under per-tensor symmetric quant*.
           Pi camera + IMX708 ISP produces a different activation
           distribution from coco128; some mid-backbone layers
           saturate while others underuse. Fixing requires HW-frame
           recalibration (needs SD frame-capture tooling) and likely
           per-channel quantization (~2 sessions more).
      Both `USE_INT8=1` (Tier 1 W8A32, V173) and `USE_INT8_W8A8=1`
      (V177) builds are retained as alternative flags — code preserved,
      not default. Code is also a useful starting point if the project
      ever moves to a Pi 4/5 (A72/A76, ARMv8.2 with SDOT) where int8
      MUL is hardware-accelerated and the economics flip.
  - **Tier 3 (A53-specific layouts) — ×3-4, ≥2 weeks.** Not pursued.
- **B4 — Frame-skip detection.** Now the primary throughput lever
  after Tier 2 closed. Camera + HUD continue rendering at their HW
  rate (~16 fps on the current pipeline) while YOLO inference runs
  once every N frames; the last detection's bboxes persist on screen
  until refreshed. Trade-off: bbox staleness × N (e.g. at N=10,
  detections lag the scene by ~0.7 s) for an order-of-magnitude
  UX win on perceived smoothness.
  - [ ] Add `FRAME_SKIP_N ?= 1` Makefile flag → `YOLO_INFER_EVERY_N`.
        N=1 = no change; N>1 = skip inference on (N-1) of N frames.
  - [ ] Split `run_yolo_complete` so the capture + render path runs
        every frame; the heavy graph (preprocess + backbone + neck +
        head + NMS) runs only on the chosen interval. `preds[]` is
        the cache between inferences.
  - [ ] HW measurement of effective frame cadence with N=4 and N=10.
- [ ] Stop point: ~16 fps real (camera/HUD) at any N; inference fps
      = 1.4 / N (e.g. N=10 → real inference 0.14 fps but bboxes
      refreshed at 16 fps). A53 bare-metal target tops out here.
- **V183 (2026-09-03) — first real per-layer HW profile + P8.** With the
  new bench (G3) the fp32 graph at `YOLO_IN=256`, 600 MHz, `test_image`:
  **1276 ms/frame** — `bb=502 neck=289 head=431`. Where it goes: **P3 head
  260 ms, L2 C2f @S4 154, P4 head 121, L15 C2f @S8 112, L4 82** — the three
  DFL heads are **34%** of the frame and the two highest-resolution C2f
  blocks another 21%. **P8 weight-stationary conv3×3** (ported from D2M:
  `ci` outer loop in `conv2d_partial_8ch`, each ci's weights read once per
  tile instead of once per output pixel) measured **1359 → 1276 ms (−6.1%)**
  A/B on HW, all of it in 8-ch K=3/s1 layers (P3 head −60, L4 −14, L15 −7).
  L2 is untouched because its 16-ch bottleneck runs the 4-ch kernel.
  - [ ] Port P8 to `conv2d_partial` (4-ch) → L2 (154 ms) is the target.
  - [ ] Heads: 6 convs × 3 levels = 431 ms. Model-level lever (prune P3 or
        share the cv2/cv3 stems); kernel-level is near the A53 f32 limit
        (D2M measured: store layout and FMLA scheduling changes = 0 ms).
  - [ ] Winograd F(2×2,3×3) for the remaining 3×3s (~1.8× on those layers).
  - Not a lever: INT8 (Tier 2 verdict above), FMLA interleave, contiguous
    output store — all measured 0 ms on this SoC.
- **V184 (2026-09-03) — `test_image.bin` was a 320² fossil.** Since V169
  (`YOLO_IN` 320→256) the tensor stayed 3×320×320; the kernel reads the first
  3·N² floats, so the colour planes were misaligned and the deterministic
  path saw striped garbage — `[DET] none` with huge class logits. Every
  test-image result from V169 to V183 (incl. the W8A8 "calibration mismatch"
  diagnosis, which used this input) should be re-read with that in mind.
  Fixed by regenerating from `bus.jpg` at `YOLO_IN` (`tools/convert_image.py`
  now reads it from `bsp/bsp.h`). fp32 now detects **person 88 / bus 85 /
  person 77 / person 71** — the same four objects as YOLOv5n@320 in the old
  clone. Goldens regenerated; the bench is now a real YOLO check, not just a
  kernel-numerics check.

### G3 — Reproducibility and toolchain stability

- [ ] CI sanity build (Docker-based, current `rpi-forge` image) on every
      commit. Even just `make kernel.o` per file catches drift.
- [ ] CRC-verified weights blob at boot (`weights_int8_crc.h` already
      seeded — wire the runtime check).
- [ ] One README-tested cold-start path: `git clone` → `make` →
      QEMU run → `./flash.sh` on an SD card.
- [x] **V183 (2026-09-03) — unattended hardware bench** (`make bench`,
      `tools/HWBENCH.md`). No SD swaps: a 936 B `raspbootin64` stub on the SD
      chain-loads the ~80 KB `-DSERIAL_BOOT` code kernel over UART; FP32
      weights + `test_image` ride the SD as `d2m_data.bin` loaded by the VPU at
      `0x08000000` via `initramfs`; reset is the adapter's RTS → RUN pad.
      `tools/hwbench.py` parses `[T]`/`[P]`/per-layer profile and diffs
      `[DET]` + a 16-checkpoint `[ABS]` fingerprint (fp32 `LOG_ABSMAX`,
      SERIAL_BOOT only) against goldens generated in QEMU — QEMU and HW agree
      to the thousandth. Iteration = edit → `make bench` → ~90 s → PASS/FAIL +
      profile. Gotchas baked in: stub UART clock pinned to 48 MHz to match
      `uart_init()`; reset must be RTS-only (power-cycling re-enumerates the
      CH340); camera skipped in the bench build so `test_image` is
      deterministic. Ported from the retired `~/projects/D2M` clone.
- [x] **V185 (2026-09-04) — real camera frame as the golden input.**
      `make capture` streams the exact post-ISP model input of a live frame
      over UART (base64 + CRC32) → `test_image.bin`. Closes the loop the W8A8
      post-mortem asked for ("calibration needs HW-frame recapture"): the
      deterministic bench can now run on a genuine sensor frame, and the same
      capture is the right input for any future calibration. No SD driver
      needed. First HW capture verified (CRC OK, live person 67%).
      - [x] **V188 — camera exposure fixed via the capture loop.** The
            "blown whites" were AE overexposure in high-contrast scenes (half
            the sensor ≥ 250 raw with the mean on target), not white balance;
            highlight protection in `ae_step()` brought R/B clipping from
            42/45 % to 4/6 %. Grey-world AWB added (`AWB=0` to freeze).
            `make capture` + per-channel clip stats was the instrument that
            made this a 3-iteration fix instead of guesswork.

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
