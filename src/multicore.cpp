/* File: src/multicore.cpp — 4-core parallel conv2d (bare-metal, no OS).
 *
 * See multicore.h for the synchronisation protocol.
 */
#include "multicore.h"
#include <stdint.h>
#include <arm_neon.h>

/* ------------------------------------------------------------------ */
/* Shared synchronisation state                                         */
/* ------------------------------------------------------------------ */

volatile ParallelConvTask parallel_task;
volatile int              task_epoch   = 0;
volatile int              done_count   = 0;
volatile int              cores_ready  = 0;  /* how many workers reached the loop */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static inline int get_core_id() {
    uint64_t v;
    asm volatile("mrs %0, mpidr_el1" : "=r"(v));
    return (int)(v & 3);
}

/* ------------------------------------------------------------------ */
/* conv2d_partial — NEON 4-ch kernel for a slice of output-ch groups.
 *
 * Processes groups [grp_start, grp_end) of the repacked weight tensor.
 * Equivalent to the inner loop of conv2d_neon_4ch().
 * ------------------------------------------------------------------ */
static void conv2d_partial(
        const float* in,  int H_in, int W_in, int C_in,
        const float* w_rep, int K, int stride, int pad,
        float* out,
        int grp_start, int grp_end)
{
    int H_out  = (H_in + 2 * pad - K) / stride + 1;
    int W_out  = (W_in + 2 * pad - K) / stride + 1;
    int HW_out = H_out * W_out;
    int HW_in  = H_in  * W_in;
    int CinKK  = C_in  * K * K;

    for (int g = grp_start; g < grp_end; g++) {
        int         co = g * 4;
        const float* wg = w_rep + (long)g * CinKK * 4;

        float* o0 = out + (co + 0) * HW_out;
        float* o1 = out + (co + 1) * HW_out;
        float* o2 = out + (co + 2) * HW_out;
        float* o3 = out + (co + 3) * HW_out;

        for (int oy = 0; oy < H_out; oy++) {
            for (int ox = 0; ox < W_out; ox++) {
                float32x4_t acc = vdupq_n_f32(0.0f);

                for (int ci = 0; ci < C_in; ci++) {
                    const float* in_ch = in + ci * HW_in;
                    const float* wci   = wg + ci * K * K * 4;

                    for (int ky = 0; ky < K; ky++) {
                        int iy = oy * stride + ky - pad;
                        for (int kx = 0; kx < K; kx++) {
                            int ix = ox * stride + kx - pad;
                            if ((unsigned)iy < (unsigned)H_in &&
                                (unsigned)ix < (unsigned)W_in) {
                                float       in_val = in_ch[iy * W_in + ix];
                                float32x4_t wv     = vld1q_f32(wci + (ky * K + kx) * 4);
                                acc = vmlaq_n_f32(acc, wv, in_val);
                            }
                        }
                    }
                }

                int pos  = oy * W_out + ox;
                o0[pos]  = vgetq_lane_f32(acc, 0);
                o1[pos]  = vgetq_lane_f32(acc, 1);
                o2[pos]  = vgetq_lane_f32(acc, 2);
                o3[pos]  = vgetq_lane_f32(acc, 3);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* secondary_main — entry point for cores 1-3 after EL2→EL1 setup.   */
/* ------------------------------------------------------------------ */

extern "C" void secondary_main()
{
    int core_id  = get_core_id();   /* 1, 2, or 3 */

    /* Announce that this core reached the worker loop. */
    __atomic_fetch_add((int*)&cores_ready, 1, __ATOMIC_RELEASE);
    asm volatile("sev");

    int my_epoch = 0;

    while (1) {
        /* Wait until core 0 increments task_epoch. */
        while (__atomic_load_n((int*)&task_epoch, __ATOMIC_ACQUIRE) == my_epoch) {
            asm volatile("wfe");
        }
        my_epoch = task_epoch;

        /* Snapshot task parameters (all stores before RELEASE on core 0
         * are visible after our ACQUIRE on task_epoch). */
        const float* in     = (const float*)parallel_task.in;
        int          H_in   = parallel_task.H_in;
        int          W_in   = parallel_task.W_in;
        int          C_in   = parallel_task.C_in;
        const float* w_rep  = (const float*)parallel_task.w_rep;
        int          K      = parallel_task.K;
        int          stride = parallel_task.stride;
        int          pad    = parallel_task.pad;
        float*       out    = (float*)parallel_task.out;
        int          n_grp  = parallel_task.n_grp;

        /* This core's slice of output-channel groups. */
        int slice     = n_grp / 4;           /* groups per core */
        int grp_start = core_id * slice;
        int grp_end   = grp_start + slice;

        conv2d_partial(in, H_in, W_in, C_in,
                       w_rep, K, stride, pad,
                       out, grp_start, grp_end);

        /* Signal completion (RELEASE so core 0's ACQUIRE sees our writes). */
        __atomic_fetch_add((int*)&done_count, 1, __ATOMIC_RELEASE);
        asm volatile("sev");
    }
}

/* ------------------------------------------------------------------ */
/* parallel_conv2d — called by core 0 to dispatch work to all 4 cores. */
/* ------------------------------------------------------------------ */

/* Cached result of the one-time multicore availability probe.
 *  -1 = not yet probed,  0 = unavailable (QEMU / single-core),  1 = available */
static int multicore_available = -1;

void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                     const float* w_rep, int C_out, int K,
                     int stride, int pad, float* out)
{
    int n_grp = C_out / 4;   /* C_out must be divisible by 4 */
    int H_out = (H_in + 2 * pad - K) / stride + 1;
    int W_out = (W_in + 2 * pad - K) / stride + 1;

    /* First call: probe whether secondary cores are alive.
     * On real hardware they reach secondary_main() within a few µs.
     * On QEMU raspi3b they are never started → cores_ready stays 0.
     * Spin for ~500K iterations (~500µs @ 1 GHz) then decide. */
    if (multicore_available == -1) {
        int spin = 500000;
        while (__atomic_load_n((int*)&cores_ready, __ATOMIC_ACQUIRE) < 3
               && --spin > 0) {
            asm volatile("nop");
        }
        multicore_available = (cores_ready >= 3) ? 1 : 0;
    }

    /* Fallback: QEMU or single-core environment — run on core 0 only. */
    if (!multicore_available) {
        for (int i = 0; i < C_out * H_out * W_out; i++) out[i] = 0.0f;
        conv2d_partial(in, H_in, W_in, C_in,
                       w_rep, K, stride, pad, out, 0, n_grp);
        return;
    }

    /* Zero the full output buffer (single-threaded, before workers write). */
    for (int i = 0; i < C_out * H_out * W_out; i++) out[i] = 0.0f;

    /* Fill task descriptor. */
    parallel_task.in     = in;
    parallel_task.H_in   = H_in;
    parallel_task.W_in   = W_in;
    parallel_task.C_in   = C_in;
    parallel_task.w_rep  = w_rep;
    parallel_task.C_out  = C_out;
    parallel_task.K      = K;
    parallel_task.stride = stride;
    parallel_task.pad    = pad;
    parallel_task.out    = out;
    parallel_task.n_grp  = n_grp;

    /* Reset completion counter BEFORE publishing the task. */
    __atomic_store_n((int*)&done_count, 0, __ATOMIC_RELAXED);

    /* Publish task: RELEASE ensures all stores above are visible. */
    asm volatile("dsb ish" : : : "memory");
    __atomic_fetch_add((int*)&task_epoch, 1, __ATOMIC_RELEASE);
    asm volatile("sev");   /* wake cores 1-3 from WFE */

    /* Core 0 processes groups 0 .. (n_grp/4 - 1). */
    int slice = n_grp / 4;
    conv2d_partial(in, H_in, W_in, C_in,
                   w_rep, K, stride, pad,
                   out, 0, slice);

    /* Wait for all workers to finish (ACQUIRE syncs their writes). */
    while (__atomic_load_n((int*)&done_count, __ATOMIC_ACQUIRE) < 3) {
        asm volatile("wfe");
    }
    asm volatile("dsb ish" : : : "memory");
}
