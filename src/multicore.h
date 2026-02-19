/* File: src/multicore.h — 4-core parallel conv2d for BCM2837 bare-metal.
 *
 * Synchronisation model (no OS, no POSIX):
 *
 *   Core 0 (orchestrator):
 *     1. Fill parallel_task descriptor
 *     2. Reset done_count to 0
 *     3. dsb ish + RELEASE-increment task_epoch + sev  → wakes workers
 *     4. Do its own share of the work (groups 0 .. n_grp/4-1)
 *     5. Spin on done_count until == 3 (ACQUIRE)
 *
 *   Cores 1-3 (workers): infinite loop —
 *     1. ACQUIRE-poll task_epoch until it changes
 *     2. Read parallel_task snapshot
 *     3. conv2d_partial for groups [core_id*(n_grp/4) .. (core_id+1)*(n_grp/4))
 *     4. RELEASE-increment done_count + sev
 *
 * Channel split for our network (all C_out divisible by 16 = 4 cores × 4 NEON):
 *   C_out= 16: n_grp=4  → each core: 1 group  (4  output channels)
 *   C_out= 32: n_grp=8  → each core: 2 groups (8  output channels)
 *   C_out= 64: n_grp=16 → each core: 4 groups (16 output channels)
 */
#ifndef MULTICORE_H
#define MULTICORE_H

#include <stdint.h>

/* Task descriptor filled by core 0 before each dispatch. */
struct ParallelConvTask {
    const float* in;
    int H_in, W_in, C_in;
    const float* w_rep;          /* NEON-repacked [C_out/4][C_in*K*K][4] */
    int C_out, K, stride, pad;
    float* out;
    int n_grp;                   /* C_out / 4 */
};

/* Shared synchronisation state (in .bss, zero-initialised at boot). */
extern volatile ParallelConvTask parallel_task;
extern volatile int              task_epoch;    /* monotonic counter, RELEASE writes  */
extern volatile int              done_count;    /* worker completion counter           */
extern volatile int              cores_ready;   /* secondary cores that reached worker loop */

/* Set to 1 by core 0 (in kernel_main) after BSS is cleared.
 * Lives in .data so secondary cores can safely poll it before BSS clear. */
extern "C" volatile int bss_ready;

/* Called from secondary cores via start.s.  Never returns. */
extern "C" void secondary_main();

/* Dispatch a parallel conv2d across all 4 cores.
 * Core 0 does its own slice, then waits for cores 1-3.
 * Identical semantics to conv2d_neon_4ch(); C_out must be divisible by 16. */
void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                     const float* w_rep, int C_out, int K,
                     int stride, int pad, float* out);

#endif /* MULTICORE_H */
