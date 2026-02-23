/* File: src/multicore.h — 4-core parallel conv2d/conv1x1 for BCM2837 bare-metal.
 *
 * Synchronisation model (no OS, no POSIX):
 *
 *   Core 0 (orchestrator):
 *     1. Fill parallel_task descriptor + set type
 *     2. Reset done_count to 0
 *     3. dsb ish + RELEASE-increment task_epoch + sev  → wakes workers
 *     4. Do its own share of the work
 *     5. Spin on done_count until == 3 (ACQUIRE)
 *
 *   Cores 1-3 (workers): infinite loop —
 *     1. ACQUIRE-poll task_epoch until it changes
 *     2. Read parallel_task snapshot
 *     3. Execute assigned slice (conv2d_partial or conv1x1_kernel)
 *     4. RELEASE-increment done_count + sev
 *
 * QEMU compatibility:
 *   parallel_conv2d() probes cores_ready on first call (~500K spin).
 *   If < 3 secondary cores alive (QEMU doesn't start them), falls back
 *   to single-core NEON transparently. Same output, no deadlock.
 */
#ifndef MULTICORE_H
#define MULTICORE_H

#include <stdint.h>

/* Task type discriminant */
#define TASK_CONV2D  0
#define TASK_CONV1X1 1

/* Unified task descriptor filled by core 0 before each dispatch. */
struct ParallelTask {
    int          type;           /* TASK_CONV2D or TASK_CONV1X1 */
    const float* in;
    float*       out;
    int          H, W, C_in, C_out;

    /* Conv2d-specific (type == TASK_CONV2D) */
    const float* w_rep;          /* NEON-repacked [C_out/4][C_in*K*K][4] */
    int          K, stride, pad;
    int          n_grp;          /* C_out / 4 */

    /* Conv1x1-specific (type == TASK_CONV1X1) */
    const float* w1x1;
    const float* bias;
};

/* Shared synchronisation state (in .bss, zero-initialised at boot). */
extern volatile ParallelTask parallel_task;
extern volatile int          task_epoch;   /* monotonic counter, RELEASE writes  */
extern volatile int          done_count;   /* worker completion counter           */
extern volatile int          cores_ready;  /* secondary cores that reached worker loop */

/* Set to 1 by core 0 (in kernel_main) after BSS is cleared.
 * Lives in .data (start.s) so secondary cores can safely poll it before BSS clear. */
extern "C" volatile int bss_ready;

/* Called from secondary cores via start.s.  Never returns. */
extern "C" void secondary_main();

/* Dispatch a parallel conv2d across all 4 cores.
 * C_out must be divisible by 4.  Falls back to single-core if QEMU. */
void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                     const float* w_rep, int C_out, int K,
                     int stride, int pad, float* out);

/* Dispatch a parallel conv1x1 (matrix multiply form) across all 4 cores.
 * C_out must be divisible by 4.  Falls back to single-core if QEMU. */
void parallel_conv1x1(const float* in, int H, int W, int C_in,
                      const float* w, const float* b, int C_out, float* out);

/* flush_to_ram — still used by mailbox.cpp for GPU coherency. */
extern "C" void flush_to_ram(volatile void* addr, unsigned long size);

#endif /* MULTICORE_H */
