/* File: src/multicore.cpp — 4-core parallel inference engine (bare-metal, no OS). */
#include "multicore.h"
#include "mmu.h"
#include <stdint.h>
#include <arm_neon.h>

/* ------------------------------------------------------------------ */
/* External functions                                                   */
/* ------------------------------------------------------------------ */

extern "C" void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in,
                                         const float* w, const float* b,
                                         int C_out_start, int C_out_end, int C_out_total,
                                         float* out);

/* ------------------------------------------------------------------ */
/* Shared synchronisation state                                         */
/* ------------------------------------------------------------------ */

volatile ParallelTask parallel_task;
volatile int          task_epoch  = 0;
volatile int          done_count  = 0;
volatile int          cores_ready = 0;   /* secondary cores that reached worker loop */

/* ------------------------------------------------------------------ */
/* Cached multicore availability (probed on first call)                */
/* ------------------------------------------------------------------ */

static int multicore_available = -1;   /* -1 = not probed yet */

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
 * Supports arbitrary kernel size K (not just K=3).
 * Weight layout: [C_out/4][C_in*K*K][4] (repacked by export_model.py).
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
        int          co = g * 4;
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

                int pos = oy * W_out + ox;
                o0[pos] = vgetq_lane_f32(acc, 0);
                o1[pos] = vgetq_lane_f32(acc, 1);
                o2[pos] = vgetq_lane_f32(acc, 2);
                o3[pos] = vgetq_lane_f32(acc, 3);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* secondary_main — entry point for cores 1-3 after EL2→EL1 setup.   */
/* ------------------------------------------------------------------ */

extern "C" void secondary_main()
{
    int core_id = get_core_id();   /* 1, 2, or 3 */

    /* Enable D-cache + I-cache on this core.
     * mmu.cpp's secondary branch waits for Core 0 to fill the translation
     * table (mmu_table_ready flag), then configures this core's own
     * MAIR_EL1 / TCR_EL1 / TTBR0_EL1 and sets SCTLR_EL1.M+C+I.
     * Without this, secondary cores hit LPDDR2 uncached on every tensor
     * access, making them ~10× slower than Core 0 and ADDING latency. */
    init_mmu();

    /* Announce that this core reached the worker loop.
     * Core 0 waits for cores_ready == 3 on first parallel_conv2d call. */
    __atomic_fetch_add((int*)&cores_ready, 1, __ATOMIC_RELEASE);
    asm volatile("sev");

    int my_epoch = 0;

    while (1) {
        /* Wait until core 0 increments task_epoch (wfe saves power + yields in QEMU). */
        while (__atomic_load_n((int*)&task_epoch, __ATOMIC_ACQUIRE) == my_epoch) {
            asm volatile("wfe");
        }
        my_epoch = task_epoch;

        /* Read task type and parameters (all visible after ACQUIRE on task_epoch). */
        int type = parallel_task.type;

        if (type == TASK_CONV2D) {
            const float* in    = (const float*)parallel_task.in;
            int          H_in  = parallel_task.H;
            int          W_in  = parallel_task.W;
            int          C_in  = parallel_task.C_in;
            const float* w_rep = (const float*)parallel_task.w_rep;
            int          K     = parallel_task.K;
            int          stride= parallel_task.stride;
            int          pad   = parallel_task.pad;
            float*       out   = (float*)parallel_task.out;
            int          n_grp = parallel_task.n_grp;

            int slice     = n_grp / 4;
            int grp_start = core_id * slice;
            int grp_end   = grp_start + slice;

            conv2d_partial(in, H_in, W_in, C_in, w_rep, K, stride, pad, out, grp_start, grp_end);
        }
        else if (type == TASK_CONV1X1) {
            const float* in   = (const float*)parallel_task.in;
            int          H    = parallel_task.H;
            int          W    = parallel_task.W;
            int          C_in = parallel_task.C_in;
            const float* w    = (const float*)parallel_task.w1x1;
            const float* b    = (const float*)parallel_task.bias;
            float*       out  = (float*)parallel_task.out;
            int          C_out= parallel_task.C_out;

            int chunk = C_out / 4;
            int start = core_id * chunk;
            int end   = (core_id == 3) ? C_out : (start + chunk);

            ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, start, end, C_out, out);
        }

        /* Signal completion (RELEASE so core 0's ACQUIRE sees our writes). */
        __atomic_fetch_add((int*)&done_count, 1, __ATOMIC_RELEASE);
        asm volatile("sev");
    }
}

/* ------------------------------------------------------------------ */
/* probe_multicore — one-time check if secondary cores are alive.      */
/* ------------------------------------------------------------------ */

static void probe_multicore() {
    if (multicore_available != -1) return;

    /* On real hardware secondary cores reach secondary_main within a few µs.
     * On QEMU raspi3b they are never started → cores_ready stays 0.
     * Spin ~500K iterations (~500µs @ 1GHz), then decide. */
    int spin = 500000;
    while (__atomic_load_n((int*)&cores_ready, __ATOMIC_ACQUIRE) < 3 && --spin > 0) {
        asm volatile("nop");
    }
    multicore_available = (cores_ready >= 3) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* dispatch_task — common publish + wait pattern for core 0.           */
/* ------------------------------------------------------------------ */

static void dispatch_task_and_wait() {
    /* Reset completion counter BEFORE publishing the task. */
    __atomic_store_n((int*)&done_count, 0, __ATOMIC_RELAXED);

    /* Publish task: dsb ish ensures all task-field stores are visible,
     * then RELEASE-increment wakes workers. */
    asm volatile("dsb ish" : : : "memory");
    __atomic_fetch_add((int*)&task_epoch, 1, __ATOMIC_RELEASE);
    asm volatile("sev");
}

static void wait_for_workers() {
    while (__atomic_load_n((int*)&done_count, __ATOMIC_ACQUIRE) < 3) {
        asm volatile("wfe");
    }
    asm volatile("dsb ish" : : : "memory");
}

/* ------------------------------------------------------------------ */
/* parallel_conv2d — dispatch conv2d across all 4 cores.               */
/* C_out must be divisible by 4. Supports arbitrary kernel size K.     */
/* ------------------------------------------------------------------ */

void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                     const float* w_rep, int C_out, int K,
                     int stride, int pad, float* out)
{
    int n_grp = C_out / 4;
    int H_out = (H_in + 2 * pad - K) / stride + 1;
    int W_out = (W_in + 2 * pad - K) / stride + 1;

    probe_multicore();

    /* Fallback: QEMU or single-core environment. */
    if (!multicore_available) {
        for (int i = 0; i < C_out * H_out * W_out; i++) out[i] = 0.0f;
        conv2d_partial(in, H_in, W_in, C_in, w_rep, K, stride, pad, out, 0, n_grp);
        return;
    }

    /* Zero output buffer (sequential, before workers write). */
    for (int i = 0; i < C_out * H_out * W_out; i++) out[i] = 0.0f;

    /* Fill task. */
    parallel_task.type   = TASK_CONV2D;
    parallel_task.in     = in;
    parallel_task.H      = H_in;
    parallel_task.W      = W_in;
    parallel_task.C_in   = C_in;
    parallel_task.C_out  = C_out;
    parallel_task.w_rep  = w_rep;
    parallel_task.K      = K;
    parallel_task.stride = stride;
    parallel_task.pad    = pad;
    parallel_task.out    = out;
    parallel_task.n_grp  = n_grp;

    dispatch_task_and_wait();

    /* Core 0 processes groups 0 .. n_grp/4-1. */
    int slice = n_grp / 4;
    conv2d_partial(in, H_in, W_in, C_in, w_rep, K, stride, pad, out, 0, slice);

    wait_for_workers();
}

/* ------------------------------------------------------------------ */
/* parallel_conv1x1 — dispatch 1x1 conv (matrix multiply) across 4    */
/* cores. C_out must be divisible by 4.                                */
/* ------------------------------------------------------------------ */

void parallel_conv1x1(const float* in, int H, int W, int C_in,
                      const float* w, const float* b, int C_out, float* out)
{
    probe_multicore();

    /* Fallback: single-core. */
    if (!multicore_available) {
        ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, C_out, C_out, out);
        return;
    }

    /* Fill task. */
    parallel_task.type  = TASK_CONV1X1;
    parallel_task.in    = in;
    parallel_task.H     = H;
    parallel_task.W     = W;
    parallel_task.C_in  = C_in;
    parallel_task.C_out = C_out;
    parallel_task.w1x1  = w;
    parallel_task.bias  = b;
    parallel_task.out   = out;

    dispatch_task_and_wait();

    /* Core 0 processes its slice. */
    int chunk = C_out / 4;
    ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, chunk, C_out, out);

    wait_for_workers();
}

/* ------------------------------------------------------------------ */
/* flush_to_ram — used by mailbox.cpp for GPU D-cache coherency.       */
/* ------------------------------------------------------------------ */

extern "C" void flush_to_ram(volatile void* addr, unsigned long size) {
    unsigned long start = (unsigned long)addr & ~0x3FUL;
    unsigned long end   = (unsigned long)addr + size;
    for (unsigned long curr = start; curr < end; curr += 64) {
        asm volatile("dc civac, %0" :: "r"(curr) : "memory");
    }
    asm volatile("dsb sy\n\tisb" ::: "memory");
}
