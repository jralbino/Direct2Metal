# Project Context: AI Direct-to-Metal

This document provides a high-level overview of the "AI Direct-to-Metal" project. For a more detailed explanation and instructions on how to build and run the code, please see the [README.md](README.md) file. For a log of project updates, see the [CHANGELOG.md](CHANGELOG.md) file.

## Project Summary

*   **Project:** AI Direct-to-Metal (YOLO on RPi Zero 2 W)
*   **Last Updated:** February 16, 2026
*   **Target Hardware:** Raspberry Pi Zero 2 W (ARM Cortex-A53)
*   **Simulation Environment:** QEMU (-M raspi3b) + Docker Toolchain

## Project Status Executive Summary
*   **Phase 1 (Compute/Math):** ✅ COMPLETE (Matrix Mult: 2.52x Speedup).
*   **Phase 2 (Video/Output):** ✅ COMPLETE (Direct Framebuffer Access).
*   **Phase 3 (AI/Inference Engine):** ✅ COMPLETE
    *   **Achievement:** Built a hybrid C++/Assembly Inference Engine from scratch.
    *   **Weights:** Successfully loaded binary weights from Python/PyTorch.
    *   **Performance:** NEON Fast Kernel (v2) achieved 51,933 cycles, beating the C++ Baseline (91,135 cycles) by 1.75x.
*   **Phase 4 (Input/Camera):** 📅 PENDING (Waiting for Hardware delivery).

## Technical Architecture (The "Zero-Layer" Stack)
*   **Boot:** `start.s` (Core 0, Stack setup).
*   **Kernel:** `kernel.cpp` (Orchestrator).
    *   Handles Mailbox (Video) and Weights loading.
    *   Manages the "Hybrid Loop" (iterating H/W in C++, Filters in ASM).
*   **AI Core:** `conv2d_neon.s`
    *   **Strategy:** "Direct Pointer Access" (No-Copy).
    *   **Parallelism:** 4 Output Channels per pass using SIMD registers (v0..v5).
    *   **Safety:** Manages LR (Link Register) manually to allow subroutines.

## Benchmark Log (The Hall of Fame)
| ID | Test Name | Implementation | Result | Status | Notes |
|---|---|---|---|---|---|
| B01 | GEMM 16x16 | C++ (-O3) | 16,106 cycles | Baseline | - |
| B02 | GEMM 16x16 | ASM NEON (v2) | 6,383 cycles | PASS | 2.52x Faster. |
| AI-01 | Conv2d (16x16) | C++ Naive | 91,135 cycles | Baseline | Slow memory access. |
| AI-02 | Conv2d (16x16) | NEON (Copy) | 186,944 cycles | FAIL | Overhead killed perf. |
| AI-03 | Conv2d (16x16) | NEON (Fast) | 51,933 cycles | WINNER | 1.75x Speedup. |

## Next Steps (Hardware Integration)
*   **Current Status:** The software stack is ready. We have a brain (AI), eyes (Video Driver), and muscle (NEON Assembly).
*   **Pending for Hardware Arrival:**
    *   Boot on Real Metal: Flash kernel8.img to an SD Card.
    *   Enable MMU/Cache: On real hardware, enabling the Data Cache is critical (will likely boost speed by another 10x).
    *   Camera Driver: Implement the MIPI CSI-2 receiver (The hardest part).
