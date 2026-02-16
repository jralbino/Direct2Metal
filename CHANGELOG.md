# Changelog

## February 16, 2026

*   **Phase 2 (Video/Output): COMPLETE**
    *   Successfully initialized the VideoCore IV GPU via the Mailbox Property Interface.
    *   Achieved a resolution of 640x480 at 32-bit depth.
    *   Verified that the CPU is correctly modifying Video RAM, even with rendering glitches in the QEMU/WSL display.
*   **Phase 3 (AI/YOLO Inference): IN PROGRESS**
    *   Set up the Python environment to export neural network weights to raw binary.
*   **Architecture Updates:**
    *   Added `mbox_call` implementation for GPU communication.
    *   Added robust memory clearing (BSS zeroing) to prevent GPU rejection.

### Benchmark & Validation Log

| ID  | Test Name   | Implementation    | Result      | Status   | Notes                                    |
| --- | ----------- | ----------------- | ----------- | -------- | ---------------------------------------- |
| B01 | GEMM 16x16  | C++ (-O3)         | 16,106 cycles | Baseline |                                          |
| B02 | GEMM 16x16  | ASM NEON (v1)     | 31,643 cycles | Slower   | Excessive RAM access inside loops.       |
| B03 | GEMM 16x16  | ASM NEON (v2)     | 6,383 cycles  | 2.52x    | Winner. Register Accumulation strategy. |


## February 15, 2026

*   **Project Start**
*   **Phase 1 (Compute/Math): COMPLETE**
    *   Achieved 2.52x speedup over GCC -O3 auto-vectorization using hand-written NEON Assembly.
    *   Proven that "Direct-to-Metal" logic outperforms standard compiler heuristics for matrix operations.


