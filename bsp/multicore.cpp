/* File: src/multicore.cpp - FINAL OPTIMIZED ENGINE (K=3 & K=6) */
#include "multicore.h"
#include "mmu.h"
#include "safety_config.h"
#include <stdint.h>
#include <arm_neon.h>

extern void uart_puts(const char* s);
extern "C" void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in,
                                         const float* w, const float* b,
                                         int C_out_start, int C_out_end, int C_out_total,
                                         bool do_silu, float* out);

volatile ParallelTask parallel_task; volatile int task_epoch=0; volatile int done_count=0; volatile int cores_ready=0;
static int multicore_available = -1;
static inline int get_core_id() { uint64_t v; asm volatile("mrs %0, mpidr_el1" : "=r"(v)); return (int)(v & 3); }

static inline float32x4_t neon_expf4(float32x4_t x) {
    x = vminq_f32(x, vdupq_n_f32(88.0f)); x = vmaxq_f32(x, vdupq_n_f32(-88.0f));
    float32x4_t z = vmulq_n_f32(x, 1.44269504f); int32x4_t k = vcvtaq_s32_f32(z);
    float32x4_t r = vmlsq_n_f32(x, vcvtq_f32_s32(k), 0.69314718f);
    float32x4_t p = vdupq_n_f32(0.00833333f);
    p = vmlaq_f32(vdupq_n_f32(0.04166667f), r, p); p = vmlaq_f32(vdupq_n_f32(0.16666667f), r, p);
    p = vmlaq_f32(vdupq_n_f32(0.5f), r, p); p = vmlaq_f32(vdupq_n_f32(1.0f), r, p); p = vmlaq_f32(vdupq_n_f32(1.0f), r, p);
    int32x4_t pow2 = vshlq_n_s32(vaddq_s32(k, vdupq_n_s32(127)), 23);
    return vmulq_f32(vreinterpretq_f32_s32(pow2), p);
}
static inline float32x4_t neon_silu(float32x4_t x) {
    float32x4_t e = neon_expf4(vnegq_f32(x)); float32x4_t denom = vaddq_f32(e, vdupq_n_f32(1.0f));
    float32x4_t recip = vrecpeq_f32(denom); recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
    recip = vmulq_f32(vrecpsq_f32(denom, recip), recip); return vmulq_f32(x, recip);
}

static void conv2d_partial(const float* in,  int H_in, int W_in, int C_in, const float* w_rep, const float* bias, int K, int stride, int pad, bool do_silu, float* out, int grp_start, int grp_end) {
    int H_out  = (H_in + 2 * pad - K) / stride + 1; int W_out  = (W_in + 2 * pad - K) / stride + 1;
    int HW_out = H_out * W_out; int HW_in  = H_in  * W_in; int CinKK  = C_in  * K * K;
    #define TILE_H 4
    #define TILE_W 8

    for (int y_tile = 0; y_tile < H_out; y_tile += TILE_H) {
        int y_end = (y_tile + TILE_H < H_out) ? y_tile + TILE_H : H_out;
        for (int x_tile = 0; x_tile < W_out; x_tile += TILE_W) {
            int x_end = (x_tile + TILE_W < W_out) ? x_tile + TILE_W : W_out;
            for (int g = grp_start; g < grp_end; g++) {
                int co = g * 4; const float* wg = w_rep + (long)g * CinKK * 4;
                float32x4_t v_bias = bias ? vld1q_f32(bias + co) : vdupq_n_f32(0.0f);
                float* o0 = out + (co + 0) * HW_out; float* o1 = out + (co + 1) * HW_out;
                float* o2 = out + (co + 2) * HW_out; float* o3 = out + (co + 3) * HW_out;

                for (int oy = y_tile; oy < y_end; oy++) {
                    for (int ox = x_tile; ox < x_end; ox++) {
                        float32x4_t acc = v_bias;

                        bool safe = (oy * stride - pad >= 0) && (oy * stride + K - pad <= H_in) &&
                                    (ox * stride - pad >= 0) && (ox * stride + K - pad <= W_in);

                        if (safe && K == 3) {
                            int base_y = oy * stride - pad; int base_x = ox * stride - pad;
                            int in_offset = base_y * W_in + base_x;
                            float32x4_t s0 = vdupq_n_f32(0.0f), s1 = vdupq_n_f32(0.0f), s2 = vdupq_n_f32(0.0f);

                            for (int ci = 0; ci < C_in; ci++) {
                                const float* in_ptr = in + ci * HW_in + in_offset;
                                const float* wci    = wg + ci * 36;
                                s0 = vmlaq_n_f32(s0, vld1q_f32(wci + 0),  in_ptr[0]);
                                s0 = vmlaq_n_f32(s0, vld1q_f32(wci + 4),  in_ptr[1]);
                                s0 = vmlaq_n_f32(s0, vld1q_f32(wci + 8),  in_ptr[2]);
                                s1 = vmlaq_n_f32(s1, vld1q_f32(wci + 12), in_ptr[W_in]);
                                s1 = vmlaq_n_f32(s1, vld1q_f32(wci + 16), in_ptr[W_in + 1]);
                                s1 = vmlaq_n_f32(s1, vld1q_f32(wci + 20), in_ptr[W_in + 2]);
                                s2 = vmlaq_n_f32(s2, vld1q_f32(wci + 24), in_ptr[W_in * 2]);
                                s2 = vmlaq_n_f32(s2, vld1q_f32(wci + 28), in_ptr[W_in * 2 + 1]);
                                s2 = vmlaq_n_f32(s2, vld1q_f32(wci + 32), in_ptr[W_in * 2 + 2]);
                            }
                            acc = vaddq_f32(acc, vaddq_f32(s0, vaddq_f32(s1, s2)));

                        } else if (safe && K == 6) {
                            int base_y = oy * stride - pad; int base_x = ox * stride - pad;
                            int in_offset = base_y * W_in + base_x;
                            float32x4_t s0 = vdupq_n_f32(0.0f);

                            for (int ci = 0; ci < C_in; ci++) {
                                const float* in_ptr = in + ci * HW_in + in_offset;
                                const float* wci    = wg + ci * 144;
                                for(int r=0; r<6; r++) {
                                    int row_off = r * W_in;
                                    int w_off = r * 24;
                                    s0 = vmlaq_n_f32(s0, vld1q_f32(wci + w_off + 0),  in_ptr[row_off + 0]);
                                    s0 = vmlaq_n_f32(s0, vld1q_f32(wci + w_off + 4),  in_ptr[row_off + 1]);
                                    s0 = vmlaq_n_f32(s0, vld1q_f32(wci + w_off + 8),  in_ptr[row_off + 2]);
                                    s0 = vmlaq_n_f32(s0, vld1q_f32(wci + w_off + 12), in_ptr[row_off + 3]);
                                    s0 = vmlaq_n_f32(s0, vld1q_f32(wci + w_off + 16), in_ptr[row_off + 4]);
                                    s0 = vmlaq_n_f32(s0, vld1q_f32(wci + w_off + 20), in_ptr[row_off + 5]);
                                }
                            }
                            acc = vaddq_f32(acc, s0);

                        } else {
                            for (int ci = 0; ci < C_in; ci++) {
                                const float* in_ch = in + ci * HW_in; const float* wci   = wg + ci * K * K * 4;
                                for (int ky = 0; ky < K; ky++) {
                                    int iy = oy * stride + ky - pad;
                                    for (int kx = 0; kx < K; kx++) {
                                        int ix = ox * stride + kx - pad;
                                        if ((unsigned)iy < (unsigned)H_in && (unsigned)ix < (unsigned)W_in) {
                                            float in_val = in_ch[iy * W_in + ix];
                                            float32x4_t wv = vld1q_f32(wci + (ky * K + kx) * 4);
                                            acc = vmlaq_n_f32(acc, wv, in_val);
                                        }
                                    }
                                }
                            }
                        }

                        if (do_silu) acc = neon_silu(acc);
                        int pos = oy * W_out + ox; o0[pos] = vgetq_lane_f32(acc, 0); o1[pos] = vgetq_lane_f32(acc, 1);
                        o2[pos] = vgetq_lane_f32(acc, 2); o3[pos] = vgetq_lane_f32(acc, 3);
                    }
                }
            }
        }
    }
}

/* conv2d_partial_8ch: 8-output-channel NEON kernel.
 * Weight layout: [C_out/8][C_in*K*K][8]  (from repack_for_neon_8ch).
 * K=3 safe path: 6 row accumulators (s0l/s0h, s1l/s1h, s2l/s2h) × 9 positions.
 * Each input scalar drives 2 vmlaq_n_f32 → 2× FMA throughput vs 4-ch. */
static void conv2d_partial_8ch(const float* in, int H_in, int W_in, int C_in,
                                const float* w_rep, const float* bias, int K,
                                int stride, int pad, bool do_silu, float* out,
                                int grp_start, int grp_end) {
    int H_out  = (H_in + 2*pad - K) / stride + 1;
    int W_out  = (W_in + 2*pad - K) / stride + 1;
    int HW_out = H_out * W_out;
    int HW_in  = H_in  * W_in;
    int CinKK  = C_in  * K * K;

    for (int y_tile = 0; y_tile < H_out; y_tile += TILE_H) {
        int y_end = (y_tile + TILE_H < H_out) ? y_tile + TILE_H : H_out;
        for (int x_tile = 0; x_tile < W_out; x_tile += TILE_W) {
            int x_end = (x_tile + TILE_W < W_out) ? x_tile + TILE_W : W_out;
            for (int g = grp_start; g < grp_end; g++) {
                int co = g * 8;
                const float* wg = w_rep + (long)g * CinKK * 8;
                float32x4_t v_bl = bias ? vld1q_f32(bias + co)     : vdupq_n_f32(0.0f);
                float32x4_t v_bh = bias ? vld1q_f32(bias + co + 4) : vdupq_n_f32(0.0f);
                float* o0 = out+(co+0)*HW_out; float* o1 = out+(co+1)*HW_out;
                float* o2 = out+(co+2)*HW_out; float* o3 = out+(co+3)*HW_out;
                float* o4 = out+(co+4)*HW_out; float* o5 = out+(co+5)*HW_out;
                float* o6 = out+(co+6)*HW_out; float* o7 = out+(co+7)*HW_out;

                for (int oy = y_tile; oy < y_end; oy++) {
                    for (int ox = x_tile; ox < x_end; ox++) {
                        float32x4_t al = v_bl, ah = v_bh;
                        bool safe = (oy*stride-pad >= 0) && (oy*stride+K-pad <= H_in) &&
                                    (ox*stride-pad >= 0) && (ox*stride+K-pad <= W_in);

                        if (safe && K == 3) {
                            int io = (oy*stride-pad)*W_in + (ox*stride-pad);
                            float32x4_t s0l=vdupq_n_f32(0.0f), s0h=vdupq_n_f32(0.0f);
                            float32x4_t s1l=vdupq_n_f32(0.0f), s1h=vdupq_n_f32(0.0f);
                            float32x4_t s2l=vdupq_n_f32(0.0f), s2h=vdupq_n_f32(0.0f);
                            for (int ci = 0; ci < C_in; ci++) {
                                /* PRFM next ci's input + weights tile (B2). */
                                if (ci + 1 < C_in) {
                                    __builtin_prefetch(in + (ci+1)*HW_in + io, 0, 3);
                                    __builtin_prefetch(wg + (ci+1) * 72, 0, 3);
                                }
                                const float* ip  = in + ci*HW_in + io;
                                const float* wci = wg + ci * 72; /* 9 pos × 8 floats */
                                /* Row 0 */
                                s0l=vmlaq_n_f32(s0l,vld1q_f32(wci+ 0),ip[0]);
                                s0h=vmlaq_n_f32(s0h,vld1q_f32(wci+ 4),ip[0]);
                                s0l=vmlaq_n_f32(s0l,vld1q_f32(wci+ 8),ip[1]);
                                s0h=vmlaq_n_f32(s0h,vld1q_f32(wci+12),ip[1]);
                                s0l=vmlaq_n_f32(s0l,vld1q_f32(wci+16),ip[2]);
                                s0h=vmlaq_n_f32(s0h,vld1q_f32(wci+20),ip[2]);
                                /* Row 1 */
                                s1l=vmlaq_n_f32(s1l,vld1q_f32(wci+24),ip[W_in]);
                                s1h=vmlaq_n_f32(s1h,vld1q_f32(wci+28),ip[W_in]);
                                s1l=vmlaq_n_f32(s1l,vld1q_f32(wci+32),ip[W_in+1]);
                                s1h=vmlaq_n_f32(s1h,vld1q_f32(wci+36),ip[W_in+1]);
                                s1l=vmlaq_n_f32(s1l,vld1q_f32(wci+40),ip[W_in+2]);
                                s1h=vmlaq_n_f32(s1h,vld1q_f32(wci+44),ip[W_in+2]);
                                /* Row 2 */
                                s2l=vmlaq_n_f32(s2l,vld1q_f32(wci+48),ip[W_in*2]);
                                s2h=vmlaq_n_f32(s2h,vld1q_f32(wci+52),ip[W_in*2]);
                                s2l=vmlaq_n_f32(s2l,vld1q_f32(wci+56),ip[W_in*2+1]);
                                s2h=vmlaq_n_f32(s2h,vld1q_f32(wci+60),ip[W_in*2+1]);
                                s2l=vmlaq_n_f32(s2l,vld1q_f32(wci+64),ip[W_in*2+2]);
                                s2h=vmlaq_n_f32(s2h,vld1q_f32(wci+68),ip[W_in*2+2]);
                            }
                            al=vaddq_f32(al,vaddq_f32(s0l,vaddq_f32(s1l,s2l)));
                            ah=vaddq_f32(ah,vaddq_f32(s0h,vaddq_f32(s1h,s2h)));
                        } else {
                            /* Border fallback: generic K, weight stride = K*K*8 per ci */
                            for (int ci = 0; ci < C_in; ci++) {
                                const float* in_ch = in + ci*HW_in;
                                const float* wci   = wg + ci * K * K * 8;
                                for (int ky = 0; ky < K; ky++) {
                                    int iy = oy*stride + ky - pad;
                                    for (int kx = 0; kx < K; kx++) {
                                        int ix = ox*stride + kx - pad;
                                        if ((unsigned)iy < (unsigned)H_in && (unsigned)ix < (unsigned)W_in) {
                                            float iv = in_ch[iy*W_in + ix];
                                            int woff = (ky*K+kx) * 8;
                                            al=vmlaq_n_f32(al,vld1q_f32(wci+woff),  iv);
                                            ah=vmlaq_n_f32(ah,vld1q_f32(wci+woff+4),iv);
                                        }
                                    }
                                }
                            }
                        }

                        if (do_silu) { al = neon_silu(al); ah = neon_silu(ah); }
                        int pos = oy*W_out + ox;
                        o0[pos]=vgetq_lane_f32(al,0); o1[pos]=vgetq_lane_f32(al,1);
                        o2[pos]=vgetq_lane_f32(al,2); o3[pos]=vgetq_lane_f32(al,3);
                        o4[pos]=vgetq_lane_f32(ah,0); o5[pos]=vgetq_lane_f32(ah,1);
                        o6[pos]=vgetq_lane_f32(ah,2); o7[pos]=vgetq_lane_f32(ah,3);
                    }
                }
            }
        }
    }
}

/* S6: verify per-core stack canary written by start.s at boot.
 * Canary is at (sp_base - 64KB) for each core.
 * Only called from dispatch path where multicore_available==1 (all cores started). */
static void check_stack_canary(int core_id) {
    const unsigned long bases[4] = {0x80000, 0x70000, 0x60000, 0x50000};
    const unsigned long* canary = (const unsigned long*)(bases[core_id] - 0x10000);
    SAFETY_ASSERT(*canary == STACK_CANARY_MAGIC, "stack overflow detected");
}

extern "C" void secondary_main() {
    int core_id = get_core_id(); init_mmu(); __atomic_fetch_add((int*)&cores_ready, 1, __ATOMIC_RELEASE); asm volatile("sev");
    int my_epoch = 0;
    while (1) {
        /* P5: exponential backoff — start at 10 yields, double up to 1024 */
        unsigned int backoff = 10;
        while (__atomic_load_n((int*)&task_epoch, __ATOMIC_ACQUIRE) == my_epoch) {
            for (unsigned int i = 0; i < backoff; i++) asm volatile("yield");
            if (backoff < 1024) backoff *= 2;
        }
        my_epoch = task_epoch;
        int type = parallel_task.type;
        if (type == TASK_CONV2D) {
            int C_out_w = parallel_task.C_out;
            int grp_w = (C_out_w >= 32 && (C_out_w & 7) == 0) ? 8 : 4;
            int slice = parallel_task.n_grp / 4; int grp_start = core_id * slice; int grp_end = grp_start + slice;
            if (grp_w == 8)
                conv2d_partial_8ch((const float*)parallel_task.in, parallel_task.H, parallel_task.W, parallel_task.C_in,
                                   (const float*)parallel_task.w_rep, (const float*)parallel_task.bias, parallel_task.K,
                                   parallel_task.stride, parallel_task.pad, parallel_task.do_silu,
                                   (float*)parallel_task.out, grp_start, grp_end);
            else
                conv2d_partial((const float*)parallel_task.in, parallel_task.H, parallel_task.W, parallel_task.C_in,
                               (const float*)parallel_task.w_rep, (const float*)parallel_task.bias, parallel_task.K,
                               parallel_task.stride, parallel_task.pad, parallel_task.do_silu,
                               (float*)parallel_task.out, grp_start, grp_end);
        } else if (type == TASK_CONV1X1) {
            int chunk = parallel_task.C_out / 4; int start = core_id * chunk; int end = (core_id == 3) ? parallel_task.C_out : (start + chunk);
            ops_neon_conv1x1_kernel((const float*)parallel_task.in, parallel_task.H, parallel_task.W, parallel_task.C_in,
                                    (const float*)parallel_task.w1x1, (const float*)parallel_task.bias, start, end,
                                    parallel_task.C_out, parallel_task.do_silu, (float*)parallel_task.out);
        }
        __atomic_fetch_add((int*)&done_count, 1, __ATOMIC_RELEASE); asm volatile("sev");
    }
}

static void probe_multicore() {
    if (multicore_available != -1) return; int spin = 50000000;
    while (__atomic_load_n((int*)&cores_ready, __ATOMIC_ACQUIRE) < 3 && --spin > 0) asm volatile("yield");
    multicore_available = (cores_ready >= 3) ? 1 : 0;
}

static void dispatch_task_and_wait() {
    /* S6: verify stack canaries for all 4 cores before every dispatch.
     * Safe to call here: this path is only reached when multicore_available==1,
     * meaning all secondary cores have run start.s and written their canaries. */
    check_stack_canary(0);
    check_stack_canary(1);
    check_stack_canary(2);
    check_stack_canary(3);
    __atomic_store_n((int*)&done_count, 0, __ATOMIC_RELAXED); asm volatile("dsb ish" : : : "memory");
    __atomic_fetch_add((int*)&task_epoch, 1, __ATOMIC_RELEASE); asm volatile("sev");
}

/* S3: finite timeout on worker wait — degrades to single-core on hang */
static void wait_for_workers() {
    unsigned long timeout = DISPATCH_TIMEOUT;
    while (__atomic_load_n((int*)&done_count, __ATOMIC_ACQUIRE) < 3) {
        asm volatile("yield");
        if (--timeout == 0) {
            uart_puts("[SAFETY] worker timeout — degrading to single-core\r\n");
            multicore_available = 0;
            return;
        }
    }
    asm volatile("dsb ish" : : : "memory");
}

void parallel_conv2d(const float* in, int H_in, int W_in, int C_in, const float* w_rep, const float* bias, int C_out, int K, int stride, int pad, bool do_silu, float* out) {
    int grp_w = (C_out >= 32 && (C_out & 7) == 0) ? 8 : 4;
    int n_grp = C_out / grp_w; probe_multicore();
    if (!multicore_available) {
        if (grp_w == 8) conv2d_partial_8ch(in, H_in, W_in, C_in, w_rep, bias, K, stride, pad, do_silu, out, 0, n_grp);
        else            conv2d_partial(in, H_in, W_in, C_in, w_rep, bias, K, stride, pad, do_silu, out, 0, n_grp);
        return;
    }
    parallel_task.type = TASK_CONV2D; parallel_task.in = in; parallel_task.H = H_in; parallel_task.W = W_in;
    parallel_task.C_in = C_in; parallel_task.C_out = C_out; parallel_task.w_rep = w_rep; parallel_task.bias = bias;
    parallel_task.K = K; parallel_task.stride = stride; parallel_task.pad = pad; parallel_task.do_silu = do_silu;
    parallel_task.out = out; parallel_task.n_grp = n_grp;
    dispatch_task_and_wait();
    int slice = n_grp / 4;
    if (grp_w == 8) conv2d_partial_8ch(in, H_in, W_in, C_in, w_rep, bias, K, stride, pad, do_silu, out, 0, slice);
    else            conv2d_partial(in, H_in, W_in, C_in, w_rep, bias, K, stride, pad, do_silu, out, 0, slice);
    wait_for_workers();
}

void parallel_conv1x1(const float* in, int H, int W, int C_in, const float* w, const float* b, int C_out, bool do_silu, float* out) {
    probe_multicore();
    if (!multicore_available) { ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, C_out, C_out, do_silu, out); return; }
    parallel_task.type = TASK_CONV1X1; parallel_task.in = in; parallel_task.H = H; parallel_task.W = W;
    parallel_task.C_in = C_in; parallel_task.C_out = C_out; parallel_task.w1x1 = w; parallel_task.bias = b;
    parallel_task.do_silu = do_silu; parallel_task.out = out;
    dispatch_task_and_wait();
    int chunk = C_out / 4; ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, chunk, C_out, do_silu, out);
    wait_for_workers();
}

/* S4: NULL/zero-size guard before cache flush */
extern "C" void flush_to_ram(volatile void* addr, unsigned long size) {
    if (addr == nullptr || size == 0) return;
    unsigned long start = (unsigned long)addr & ~0x3FUL; unsigned long end   = (unsigned long)addr + size;
    for (unsigned long curr = start; curr < end; curr += 64) asm volatile("dc civac, %0" :: "r"(curr) : "memory");
    asm volatile("dsb sy\n\tisb" ::: "memory");
}
