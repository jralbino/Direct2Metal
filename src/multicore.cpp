/* File: src/multicore.cpp - CACHE-AWARE TILING ENGINE (Linker Fixed) */
#include "multicore.h"
#include "mmu.h"
#include <stdint.h>
#include <arm_neon.h>

extern void uart_puts(const char* s); // <--- ENLACE C++ NORMAL

extern "C" void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in,
                                         const float* w, const float* b,
                                         int C_out_start, int C_out_end, int C_out_total,
                                         float* out);

volatile ParallelTask parallel_task;
volatile int          task_epoch  = 0;
volatile int          done_count  = 0;
volatile int          cores_ready = 0;   

static int multicore_available = -1;   

static inline int get_core_id() {
    uint64_t v;
    asm volatile("mrs %0, mpidr_el1" : "=r"(v));
    return (int)(v & 3);
}

// --- TILING ESPACIAL PARA L2 CACHE ---
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

    #define TILE_H 16
    #define TILE_W 16

    for (int y_tile = 0; y_tile < H_out; y_tile += TILE_H) {
        int y_end = (y_tile + TILE_H < H_out) ? y_tile + TILE_H : H_out;
        
        for (int x_tile = 0; x_tile < W_out; x_tile += TILE_W) {
            int x_end = (x_tile + TILE_W < W_out) ? x_tile + TILE_W : W_out;

            for (int g = grp_start; g < grp_end; g++) {
                int          co = g * 4;
                const float* wg = w_rep + (long)g * CinKK * 4;

                float* o0 = out + (co + 0) * HW_out;
                float* o1 = out + (co + 1) * HW_out;
                float* o2 = out + (co + 2) * HW_out;
                float* o3 = out + (co + 3) * HW_out;

                for (int oy = y_tile; oy < y_end; oy++) {
                    for (int ox = x_tile; ox < x_end; ox++) {
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
    }
}

extern "C" void secondary_main()
{
    int core_id = get_core_id();   
    init_mmu();

    __atomic_fetch_add((int*)&cores_ready, 1, __ATOMIC_RELEASE);
    asm volatile("sev");

    int my_epoch = 0;

    while (1) {
        while (__atomic_load_n((int*)&task_epoch, __ATOMIC_ACQUIRE) == my_epoch) {
            asm volatile("wfe");
        }
        my_epoch = task_epoch;

        int type = parallel_task.type;

        if (type == TASK_CONV2D) {
            int slice     = parallel_task.n_grp / 4;
            int grp_start = core_id * slice;
            int grp_end   = grp_start + slice;

            conv2d_partial((const float*)parallel_task.in, parallel_task.H, parallel_task.W, parallel_task.C_in, 
                           (const float*)parallel_task.w_rep, parallel_task.K, parallel_task.stride, parallel_task.pad, 
                           (float*)parallel_task.out, grp_start, grp_end);
        }
        else if (type == TASK_CONV1X1) {
            int chunk = parallel_task.C_out / 4;
            int start = core_id * chunk;
            int end   = (core_id == 3) ? parallel_task.C_out : (start + chunk);

            ops_neon_conv1x1_kernel((const float*)parallel_task.in, parallel_task.H, parallel_task.W, parallel_task.C_in, 
                                    (const float*)parallel_task.w1x1, (const float*)parallel_task.bias, start, end, parallel_task.C_out, (float*)parallel_task.out);
        }

        __atomic_fetch_add((int*)&done_count, 1, __ATOMIC_RELEASE);
        asm volatile("sev");
    }
}

static void probe_multicore() {
    if (multicore_available != -1) return;
    int spin = 500000;
    while (__atomic_load_n((int*)&cores_ready, __ATOMIC_ACQUIRE) < 3 && --spin > 0) {
        asm volatile("nop");
    }
    multicore_available = (cores_ready >= 3) ? 1 : 0;
    
    if (multicore_available) uart_puts("\n[HW] 4-Cores Activos y Sincronizados.\n");
    else uart_puts("\n[HW] ERROR: Timeout. Ejecutando en Single-Core.\n");
}

static void dispatch_task_and_wait() {
    __atomic_store_n((int*)&done_count, 0, __ATOMIC_RELAXED);
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

void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                     const float* w_rep, int C_out, int K,
                     int stride, int pad, float* out)
{
    int n_grp = C_out / 4;
    probe_multicore();

    if (!multicore_available) {
        conv2d_partial(in, H_in, W_in, C_in, w_rep, K, stride, pad, out, 0, n_grp);
        return;
    }

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

    int slice = n_grp / 4;
    conv2d_partial(in, H_in, W_in, C_in, w_rep, K, stride, pad, out, 0, slice);

    wait_for_workers();
}

void parallel_conv1x1(const float* in, int H, int W, int C_in,
                      const float* w, const float* b, int C_out, float* out)
{
    probe_multicore();
    if (!multicore_available) {
        ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, C_out, C_out, out);
        return;
    }

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

    int chunk = C_out / 4;
    ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, chunk, C_out, out);

    wait_for_workers();
}

extern "C" void flush_to_ram(volatile void* addr, unsigned long size) {
    unsigned long start = (unsigned long)addr & ~0x3FUL;
    unsigned long end   = (unsigned long)addr + size;
    for (unsigned long curr = start; curr < end; curr += 64) {
        asm volatile("dc civac, %0" :: "r"(curr) : "memory");
    }
    asm volatile("dsb sy\n\tisb" ::: "memory");
}