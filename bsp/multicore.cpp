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

#ifndef D2M_NO_WINOGRAD
/* ── Winograd F(2×2, 3×3) — V193, CLOSED: measured a net regression on HW,
 * kept only behind `WINOGRAD=0` (default off) as a documented, working
 * starting point. See PLAN.md V193 for the full writeup; summary:
 *
 * Cuts the K=3/s1/p1 inner product from 9 taps to 16 elementwise products
 * accumulated over C_in (9/4 = 2.25× fewer FMAs per output pixel), at the
 * cost of a cheap linear transform of the 3×3 filter and 4×4 input tile.
 * Math validated standalone against a brute-force direct conv (scratchpad
 * winograd_explicit.cpp, max abs err ~2e-6, float rounding only), and the
 * NEON port is bit-correct on real HW (fingerprint 16/16 PASS). But it's
 * **slower**: 492 -> 521-523 ms total (+6%), ~+10-15% on every 3×3-heavy
 * layer regardless of C_in, in both the first cut and after vectorizing
 * the input-transform loads (4× vld1q_f32 + in-register lane extracts
 * instead of 16 independent scalar loads — zero measured difference).
 * Root cause: the M accumulator (16 taps × up to 8 tiles × 2 lo/hi halves
 * = 256 float32x4_t) has nowhere near enough of the A53's 32 NEON
 * registers to live in, so almost every accumulate is a spill load + one
 * FMA + a spill store. The existing P8 kernel's weight-stationary design
 * gets **9** FMAs per register round-trip (9 taps into 1 loaded/stored
 * accumulator); this gets 1. The arithmetic win (9/4 fewer FMAs) is smaller
 * than the load/store-ratio loss (9× fewer FMAs per touch). A real win
 * would need a GEMM-style restructure — batch tiles into the vector lanes
 * (not the 8 output channels) so U can stream from a small per-(g,ci)
 * cache while M for many tiles stays resident — a bigger rewrite, not
 * attempted here.
 *
 * The three transforms are separable (row pass then column pass) — the
 * standard F(2,3) B/G/A matrices reduce to pure ±1 combinations for
 * input/output and ±1 combinations + one 0.5 scale for the filter:
 *
 *   input  V = B^T d B      (4×4 -> 4×4, zero multiplies)
 *   filter U = G g G^T      (3×3 -> 4×4, one ×0.5 per row/col pass)
 *   output Y = A^T (U⊙V) A  (4×4 -> 2×2, zero multiplies)
 *
 * `winograd_weight_transform` runs on 8-wide channel-interleaved filter taps
 * (the existing `repack_for_neon_8ch` layout: [ci][tap*8+ch]) so one call
 * transforms all 8 output channels of a group at once. `winograd_input_transform`
 * loads each input row as one vector (shared by every output-channel group)
 * and does the column pass 4-wide; the row pass pulls lanes back out to
 * scalars since it combines *within* a row. `winograd_output_transform` is
 * 8-wide again, producing the 2×2 output tile's 8 channels. */
static inline void winograd_weight_transform(
    float32x4_t g0, float32x4_t g1, float32x4_t g2,
    float32x4_t g3, float32x4_t g4, float32x4_t g5,
    float32x4_t g6, float32x4_t g7, float32x4_t g8,
    float32x4_t U[16]) {
    float32x4_t half = vdupq_n_f32(0.5f);
    /* column pass: S[4][3], columns of g are (g0,g3,g6) (g1,g4,g7) (g2,g5,g8) */
    float32x4_t s00 = g0, s10 = vmulq_f32(vaddq_f32(vaddq_f32(g0, g3), g6), half),
                s20 = vmulq_f32(vaddq_f32(vsubq_f32(g0, g3), g6), half), s30 = g6;
    float32x4_t s01 = g1, s11 = vmulq_f32(vaddq_f32(vaddq_f32(g1, g4), g7), half),
                s21 = vmulq_f32(vaddq_f32(vsubq_f32(g1, g4), g7), half), s31 = g7;
    float32x4_t s02 = g2, s12 = vmulq_f32(vaddq_f32(vaddq_f32(g2, g5), g8), half),
                s22 = vmulq_f32(vaddq_f32(vsubq_f32(g2, g5), g8), half), s32 = g8;
    /* row pass: U[4][4] from S[i][0..2] */
    U[0]=s00;                                            U[1]=vmulq_f32(vaddq_f32(vaddq_f32(s00,s01),s02),half);
    U[2]=vmulq_f32(vaddq_f32(vsubq_f32(s00,s01),s02),half); U[3]=s02;
    U[4]=s10;                                            U[5]=vmulq_f32(vaddq_f32(vaddq_f32(s10,s11),s12),half);
    U[6]=vmulq_f32(vaddq_f32(vsubq_f32(s10,s11),s12),half); U[7]=s12;
    U[8]=s20;                                            U[9]=vmulq_f32(vaddq_f32(vaddq_f32(s20,s21),s22),half);
    U[10]=vmulq_f32(vaddq_f32(vsubq_f32(s20,s21),s22),half); U[11]=s22;
    U[12]=s30;                                           U[13]=vmulq_f32(vaddq_f32(vaddq_f32(s30,s31),s32),half);
    U[14]=vmulq_f32(vaddq_f32(vsubq_f32(s30,s31),s32),half); U[15]=s32;
}

/* V193.1: column pass vectorized across the 4 columns — one vld1q_f32 per
 * input row (r0[b..b+3] is contiguous) instead of 4 independent scalar
 * loads, and the column-pass add/sub done 4-wide in one shot instead of
 * 16 separate scalar ops. The row pass still needs per-row scalars (it
 * combines lanes *within* a row), pulled out of the vector via
 * vgetq_lane_f32 (free/near-free lane 0, cheap dup for 1-3) instead of a
 * second round of memory loads. This was the dominant cost in the first
 * cut (16 independent scalar loads on an in-order A53 have real load
 * latency; measured net regression, see PLAN.md V193). */
static inline void winograd_input_transform(
    float32x4_t d0, float32x4_t d1, float32x4_t d2, float32x4_t d3,
    float V[16]) {
    float32x4_t t0 = vsubq_f32(d0, d2);
    float32x4_t t1 = vaddq_f32(d1, d2);
    float32x4_t t2 = vsubq_f32(d2, d1);
    float32x4_t t3 = vsubq_f32(d1, d3);
    const float32x4_t T[4] = {t0, t1, t2, t3};
    for (int i = 0; i < 4; i++) {
        float t0v = vgetq_lane_f32(T[i], 0), t1v = vgetq_lane_f32(T[i], 1);
        float t2v = vgetq_lane_f32(T[i], 2), t3v = vgetq_lane_f32(T[i], 3);
        V[i*4+0] = t0v - t2v; V[i*4+1] = t1v + t2v; V[i*4+2] = t2v - t1v; V[i*4+3] = t1v - t3v;
    }
}
/* Y = A^T M A, M indexed [i*4+j] -> Y[0..3] = (0,0) (0,1) (1,0) (1,1) */
static inline void winograd_output_transform(const float32x4_t M[16], float32x4_t Y[4]) {
    float32x4_t P00=vaddq_f32(vaddq_f32(M[0],M[4]),M[8]),   P01=vaddq_f32(vaddq_f32(M[1],M[5]),M[9]);
    float32x4_t P02=vaddq_f32(vaddq_f32(M[2],M[6]),M[10]),  P03=vaddq_f32(vaddq_f32(M[3],M[7]),M[11]);
    float32x4_t P10=vsubq_f32(vsubq_f32(M[4],M[8]),M[12]),  P11=vsubq_f32(vsubq_f32(M[5],M[9]),M[13]);
    float32x4_t P12=vsubq_f32(vsubq_f32(M[6],M[10]),M[14]), P13=vsubq_f32(vsubq_f32(M[7],M[11]),M[15]);
    Y[0]=vaddq_f32(vaddq_f32(P00,P01),P02);
    Y[1]=vsubq_f32(vsubq_f32(P01,P02),P03);
    Y[2]=vaddq_f32(vaddq_f32(P10,P11),P12);
    Y[3]=vsubq_f32(vsubq_f32(P11,P12),P13);
}
#endif /* !D2M_NO_WINOGRAD */

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

                /* V191: weight-stationary K=3 fast path, clipped to the safe
                 * (padding-free) sub-rectangle of this tile — same idea as the
                 * 8-ch kernel. L2's bottleneck 3×3 (16→16) had no P8 at all. */
#ifdef D2M_NO_P8
                const bool kP8 = false;
#else
                const bool kP8 = true;
#endif
                int fy0 = y_tile > 1 ? y_tile : 1;
                int fy1 = y_end < H_out - 1 ? y_end : H_out - 1;
                int fx0 = x_tile > 1 ? x_tile : 1;
                int fx1 = x_end < W_out - 1 ? x_end : W_out - 1;
                bool ran_fast = false;
                if (kP8 && K == 3 && pad == 1 && stride == 1 && fy0 < fy1 && fx0 < fx1) {
                    int fw = fx1 - fx0, fnpos = fw * (fy1 - fy0);
                    float acc4[TILE_H * TILE_W][4];
                    for (int p = 0; p < fnpos; p++) vst1q_f32(acc4[p], vdupq_n_f32(0.0f));
                    for (int ci = 0; ci < C_in; ci++) {
                        const float* ipc = in + ci * HW_in;
                        const float* wci = wg + ci * 36;
                        if (ci + 1 < C_in) {
                            __builtin_prefetch(wg + (ci+1) * 36, 0, 3);
                            __builtin_prefetch(in + (ci+1)*HW_in + (fy0-1)*W_in + (fx0-1), 0, 3);
                        }
                        float32x4_t w0=vld1q_f32(wci+ 0), w1=vld1q_f32(wci+ 4), w2=vld1q_f32(wci+ 8);
                        float32x4_t w3=vld1q_f32(wci+12), w4=vld1q_f32(wci+16), w5=vld1q_f32(wci+20);
                        float32x4_t w6=vld1q_f32(wci+24), w7=vld1q_f32(wci+28), w8=vld1q_f32(wci+32);
                        int p = 0;
                        for (int oy = fy0; oy < fy1; oy++) {
                            const float* r0 = ipc + (oy - 1) * W_in;
                            const float* r1 = r0 + W_in;
                            const float* r2 = r1 + W_in;
                            for (int ox = fx0; ox < fx1; ox++, p++) {
                                int b = ox - 1;
                                float32x4_t a = vld1q_f32(acc4[p]);
                                a=vmlaq_n_f32(a,w0,r0[b]);   a=vmlaq_n_f32(a,w1,r0[b+1]); a=vmlaq_n_f32(a,w2,r0[b+2]);
                                a=vmlaq_n_f32(a,w3,r1[b]);   a=vmlaq_n_f32(a,w4,r1[b+1]); a=vmlaq_n_f32(a,w5,r1[b+2]);
                                a=vmlaq_n_f32(a,w6,r2[b]);   a=vmlaq_n_f32(a,w7,r2[b+1]); a=vmlaq_n_f32(a,w8,r2[b+2]);
                                vst1q_f32(acc4[p], a);
                            }
                        }
                    }
                    int p = 0;
                    for (int oy = fy0; oy < fy1; oy++) {
                        for (int ox = fx0; ox < fx1; ox++, p++) {
                            float32x4_t acc = vaddq_f32(vld1q_f32(acc4[p]), v_bias);
                            if (do_silu) acc = neon_silu(acc);
                            int pos = oy * W_out + ox;
                            o0[pos] = vgetq_lane_f32(acc, 0); o1[pos] = vgetq_lane_f32(acc, 1);
                            o2[pos] = vgetq_lane_f32(acc, 2); o3[pos] = vgetq_lane_f32(acc, 3);
                        }
                    }
                    ran_fast = true;
                }

                for (int oy = y_tile; oy < y_end; oy++) {
                    for (int ox = x_tile; ox < x_end; ox++) {
                        if (ran_fast && oy >= fy0 && oy < fy1 && ox >= fx0 && ox < fx1) continue;
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

#ifndef D2M_NO_WINOGRAD
/* V193 CLOSED — see the block comment above winograd_weight_transform() for
 * why this loses on HW. Kept as its own function (not inlined into
 * conv2d_partial_8ch) so `-DD2M_NO_WINOGRAD` removes it — Mlo/Mhi below are
 * (TILE_H/2)*(TILE_W/2)*16*2 = 256 float32x4_t; leaving that stack frame
 * reachable from conv2d_partial_8ch even behind a runtime `if (false)` cost
 * ~12 ms/frame in the extra prologue on every one of its many calls per
 * frame (measured; see PLAN.md V193).
 *
 * Computes the 2×2-tile-aligned sub-rectangle of [fy0,fy1)×[fx0,fx1) (may
 * cover one fewer row/col at odd boundaries — *out_ry1 / *out_rx1 report what
 * was actually covered so the caller's per-position fallback can pick up
 * the remainder). Returns false (nothing written) if that sub-rectangle is
 * empty, in which case *out_ry1 / *out_rx1 are left untouched. */
static bool conv2d_winograd_tile_8ch(const float* in, int W_in, int HW_in,
                                      const float* wg, int C_in,
                                      int fy0, int fy1, int fx0, int fx1, int W_out,
                                      float32x4_t v_bl, float32x4_t v_bh, bool do_silu,
                                      float* oc[8], int* out_ry1, int* out_rx1) {
    int wy_end = fy0; for (int oy = fy0; oy + 1 < fy1; oy += 2) wy_end = oy + 2;
    int wx_end = fx0; for (int ox = fx0; ox + 1 < fx1; ox += 2) wx_end = ox + 2;
    int nty = (wy_end - fy0) / 2, ntx = (wx_end - fx0) / 2;
    if (nty <= 0 || ntx <= 0) return false;
    int ntiles = nty * ntx; /* <= (TILE_H/2)*(TILE_W/2) */
    float32x4_t Mlo[16][(TILE_H/2)*(TILE_W/2)];
    float32x4_t Mhi[16][(TILE_H/2)*(TILE_W/2)];
    for (int t = 0; t < 16; t++)
        for (int p = 0; p < ntiles; p++) { Mlo[t][p] = vdupq_n_f32(0.0f); Mhi[t][p] = vdupq_n_f32(0.0f); }

    for (int ci = 0; ci < C_in; ci++) {
        const float* ipc = in + ci * HW_in;
        const float* wci = wg + ci * 72;
        if (ci + 1 < C_in) {
            __builtin_prefetch(wg + (ci+1) * 72, 0, 3);
            __builtin_prefetch(in + (ci+1)*HW_in + (fy0-1)*W_in + (fx0-1), 0, 3);
        }
        float32x4_t g0l=vld1q_f32(wci+ 0), g1l=vld1q_f32(wci+ 8), g2l=vld1q_f32(wci+16);
        float32x4_t g3l=vld1q_f32(wci+24), g4l=vld1q_f32(wci+32), g5l=vld1q_f32(wci+40);
        float32x4_t g6l=vld1q_f32(wci+48), g7l=vld1q_f32(wci+56), g8l=vld1q_f32(wci+64);
        float32x4_t g0h=vld1q_f32(wci+ 4), g1h=vld1q_f32(wci+12), g2h=vld1q_f32(wci+20);
        float32x4_t g3h=vld1q_f32(wci+28), g4h=vld1q_f32(wci+36), g5h=vld1q_f32(wci+44);
        float32x4_t g6h=vld1q_f32(wci+52), g7h=vld1q_f32(wci+60), g8h=vld1q_f32(wci+68);
        float32x4_t Ul[16], Uh[16];
        winograd_weight_transform(g0l,g1l,g2l,g3l,g4l,g5l,g6l,g7l,g8l, Ul);
        winograd_weight_transform(g0h,g1h,g2h,g3h,g4h,g5h,g6h,g7h,g8h, Uh);

        int p = 0;
        for (int ty = 0; ty < nty; ty++) {
            int oy = fy0 + ty * 2;
            const float* r0 = ipc + (oy - 1) * W_in;
            const float* r1 = r0 + W_in;
            const float* r2 = r1 + W_in;
            const float* r3 = r2 + W_in;
            for (int tx = 0; tx < ntx; tx++, p++) {
                int b = fx0 + tx * 2 - 1;
                float V[16];
                winograd_input_transform(vld1q_f32(r0+b), vld1q_f32(r1+b),
                                          vld1q_f32(r2+b), vld1q_f32(r3+b), V);
                for (int t = 0; t < 16; t++) {
                    Mlo[t][p] = vmlaq_n_f32(Mlo[t][p], Ul[t], V[t]);
                    Mhi[t][p] = vmlaq_n_f32(Mhi[t][p], Uh[t], V[t]);
                }
            }
        }
    }

    int p = 0;
    for (int ty = 0; ty < nty; ty++) {
        int oy = fy0 + ty * 2;
        for (int tx = 0; tx < ntx; tx++, p++) {
            int ox = fx0 + tx * 2;
            float32x4_t Mtl[16], Mth[16];
            for (int t = 0; t < 16; t++) { Mtl[t] = Mlo[t][p]; Mth[t] = Mhi[t][p]; }
            float32x4_t Yl[4], Yh[4];
            winograd_output_transform(Mtl, Yl);
            winograd_output_transform(Mth, Yh);
            static const int dy[4] = {0,0,1,1}, dx[4] = {0,1,0,1};
            for (int q = 0; q < 4; q++) {
                float32x4_t al = vaddq_f32(Yl[q], v_bl);
                float32x4_t ah = vaddq_f32(Yh[q], v_bh);
                if (do_silu) { al = neon_silu(al); ah = neon_silu(ah); }
                int pos = (oy + dy[q]) * W_out + (ox + dx[q]);
                oc[0][pos]=vgetq_lane_f32(al,0); oc[1][pos]=vgetq_lane_f32(al,1);
                oc[2][pos]=vgetq_lane_f32(al,2); oc[3][pos]=vgetq_lane_f32(al,3);
                oc[4][pos]=vgetq_lane_f32(ah,0); oc[5][pos]=vgetq_lane_f32(ah,1);
                oc[6][pos]=vgetq_lane_f32(ah,2); oc[7][pos]=vgetq_lane_f32(ah,3);
            }
        }
    }
    *out_ry1 = wy_end; *out_rx1 = wx_end;
    return true;
}
#endif /* !D2M_NO_WINOGRAD */

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
                float* oc[8];
                for (int c = 0; c < 8; c++) oc[c] = out + (long)(co + c) * HW_out;
                int tw = x_end - x_tile, th = y_end - y_tile, npos = tw * th;

                /* -DD2M_NO_P8 disables the fast path (A/B baseline for `make bench`). */
#ifdef D2M_NO_P8
                const bool kP8 = false;
#else
                const bool kP8 = true;
#endif
                /* -DD2M_NO_WINOGRAD falls back to the plain P8 direct-conv fast
                 * path below (A/B baseline for the Winograd feature alone). */
                (void)npos;
                /* V191: weight-stationary K=3 fast path, clipped to the "safe"
                 * (padding-free) sub-rectangle of this tile — [fy0,fy1)×[fx0,fx1).
                 * V183 gated it at whole-tile granularity (interior tiles only),
                 * which at S8=32 / TILE_W=8 left ~60 % of positions on the slow
                 * per-position path that re-reads the 288 B weight block for
                 * every pixel. Now only the 1-px padded ring falls through. */
                int fy0 = y_tile > 1 ? y_tile : 1;
                int fy1 = y_end < H_out - 1 ? y_end : H_out - 1;
                int fx0 = x_tile > 1 ? x_tile : 1;
                int fx1 = x_end < W_out - 1 ? x_end : W_out - 1;
                bool ran_fast = false;
                /* Covered sub-rectangle of THIS tile's fast path — [fy0,ry1)×[fx0,rx1).
                 * Equals [fy0,fy1)×[fx0,fx1) for the P8 path; the Winograd path below
                 * may cover one fewer row/col at the block's edge (2×2-tile parity),
                 * in which case the per-position fallback picks up the remainder. */
                int ry1 = fy1, rx1 = fx1;
#ifndef D2M_NO_WINOGRAD
                /* V193 CLOSED (see the block comment above winograd_weight_transform):
                 * measured slower on HW, kept out of the default build entirely — the
                 * whole function (including its big Mlo/Mhi arrays) must not exist
                 * under -DD2M_NO_WINOGRAD, or its stack frame taxes every call to
                 * conv2d_partial_8ch even when the runtime branch never fires. */
                if (K == 3 && pad == 1 && stride == 1 && fy0 < fy1 && fx0 < fx1) {
                    ran_fast = conv2d_winograd_tile_8ch(in, W_in, HW_in, wg, C_in,
                                                         fy0, fy1, fx0, fx1, W_out,
                                                         v_bl, v_bh, do_silu, oc, &ry1, &rx1);
                }
#endif
                if (!ran_fast && kP8 && K == 3 && pad == 1 && stride == 1 && fy0 < fy1 && fx0 < fx1) {
                    int fw = fx1 - fx0, fnpos = fw * (fy1 - fy0);
                    float accl[TILE_H * TILE_W][4];
                    float acch[TILE_H * TILE_W][4];
                    for (int p = 0; p < fnpos; p++) { vst1q_f32(accl[p], vdupq_n_f32(0.0f)); vst1q_f32(acch[p], vdupq_n_f32(0.0f)); }

                    for (int ci = 0; ci < C_in; ci++) {
                        const float* ipc = in + ci * HW_in;
                        const float* wci = wg + ci * 72;
                        if (ci + 1 < C_in) {
                            __builtin_prefetch(wg + (ci+1) * 72, 0, 3);
                            __builtin_prefetch(in + (ci+1)*HW_in + (fy0-1)*W_in + (fx0-1), 0, 3);
                        }
                        float32x4_t w0=vld1q_f32(wci+ 0), w1=vld1q_f32(wci+ 8), w2=vld1q_f32(wci+16);
                        float32x4_t w3=vld1q_f32(wci+24), w4=vld1q_f32(wci+32), w5=vld1q_f32(wci+40);
                        float32x4_t w6=vld1q_f32(wci+48), w7=vld1q_f32(wci+56), w8=vld1q_f32(wci+64);
                        float32x4_t W0=vld1q_f32(wci+ 4), W1=vld1q_f32(wci+12), W2=vld1q_f32(wci+20);
                        float32x4_t W3=vld1q_f32(wci+28), W4=vld1q_f32(wci+36), W5=vld1q_f32(wci+44);
                        float32x4_t W6=vld1q_f32(wci+52), W7=vld1q_f32(wci+60), W8=vld1q_f32(wci+68);
                        int p = 0;
                        for (int oy = fy0; oy < fy1; oy++) {
                            const float* r0 = ipc + (oy - 1) * W_in;   /* oy≥1 → valid */
                            const float* r1 = r0 + W_in;
                            const float* r2 = r1 + W_in;               /* oy≤H_out-2 → valid */
                            for (int ox = fx0; ox < fx1; ox++, p++) {
                                int b = ox - 1;                        /* ox≥1 → valid */
                                float32x4_t al = vld1q_f32(accl[p]);
                                float32x4_t ah = vld1q_f32(acch[p]);
                                al=vmlaq_n_f32(al,w0,r0[b]);   ah=vmlaq_n_f32(ah,W0,r0[b]);
                                al=vmlaq_n_f32(al,w1,r0[b+1]); ah=vmlaq_n_f32(ah,W1,r0[b+1]);
                                al=vmlaq_n_f32(al,w2,r0[b+2]); ah=vmlaq_n_f32(ah,W2,r0[b+2]);
                                al=vmlaq_n_f32(al,w3,r1[b]);   ah=vmlaq_n_f32(ah,W3,r1[b]);
                                al=vmlaq_n_f32(al,w4,r1[b+1]); ah=vmlaq_n_f32(ah,W4,r1[b+1]);
                                al=vmlaq_n_f32(al,w5,r1[b+2]); ah=vmlaq_n_f32(ah,W5,r1[b+2]);
                                al=vmlaq_n_f32(al,w6,r2[b]);   ah=vmlaq_n_f32(ah,W6,r2[b]);
                                al=vmlaq_n_f32(al,w7,r2[b+1]); ah=vmlaq_n_f32(ah,W7,r2[b+1]);
                                al=vmlaq_n_f32(al,w8,r2[b+2]); ah=vmlaq_n_f32(ah,W8,r2[b+2]);
                                vst1q_f32(accl[p], al);
                                vst1q_f32(acch[p], ah);
                            }
                        }
                    }

                    int p = 0;
                    for (int oy = fy0; oy < fy1; oy++) {
                        for (int ox = fx0; ox < fx1; ox++, p++) {
                            float32x4_t al = vaddq_f32(vld1q_f32(accl[p]), v_bl);
                            float32x4_t ah = vaddq_f32(vld1q_f32(acch[p]), v_bh);
                            if (do_silu) { al = neon_silu(al); ah = neon_silu(ah); }
                            int pos = oy*W_out + ox;
                            oc[0][pos]=vgetq_lane_f32(al,0); oc[1][pos]=vgetq_lane_f32(al,1);
                            oc[2][pos]=vgetq_lane_f32(al,2); oc[3][pos]=vgetq_lane_f32(al,3);
                            oc[4][pos]=vgetq_lane_f32(ah,0); oc[5][pos]=vgetq_lane_f32(ah,1);
                            oc[6][pos]=vgetq_lane_f32(ah,2); oc[7][pos]=vgetq_lane_f32(ah,3);
                        }
                    }
                    ran_fast = true;
                }

                for (int oy = y_tile; oy < y_end; oy++) {
                    for (int ox = x_tile; ox < x_end; ox++) {
                        if (ran_fast && oy >= fy0 && oy < ry1 && ox >= fx0 && ox < rx1) continue;
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
                        oc[0][pos]=vgetq_lane_f32(al,0); oc[1][pos]=vgetq_lane_f32(al,1);
                        oc[2][pos]=vgetq_lane_f32(al,2); oc[3][pos]=vgetq_lane_f32(al,3);
                        oc[4][pos]=vgetq_lane_f32(ah,0); oc[5][pos]=vgetq_lane_f32(ah,1);
                        oc[6][pos]=vgetq_lane_f32(ah,2); oc[7][pos]=vgetq_lane_f32(ah,3);
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
        } else if (type == TASK_CONV2D_ASYNC) {
            /* Cores 1-3 each take 1/3 of n_grp. Core 0 is busy with the
             * display loop. slice_idx = core_id - 1 ∈ {0, 1, 2}. */
            int C_out_w = parallel_task.C_out;
            int grp_w   = (C_out_w >= 32 && (C_out_w & 7) == 0) ? 8 : 4;
            int slice   = parallel_task.n_grp / 3;
            int idx     = core_id - 1;
            int grp_start = idx * slice;
            int grp_end   = (idx == 2) ? parallel_task.n_grp : (grp_start + slice);
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
        } else if (type == TASK_CONV1X1_ASYNC) {
            int chunk = parallel_task.C_out / 3;
            int idx   = core_id - 1;
            int start = idx * chunk;
            int end   = (idx == 2) ? parallel_task.C_out : (start + chunk);
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

/* ── B4 async dispatch ────────────────────────────────────────────────────
 * Same task fields, different worker code path: cores 1-3 each grab 1/3
 * of the work, core 0 returns from start() with no slice to do. The
 * caller polls parallel_async_done() and runs display work in the gap. */

static void dispatch_task_async() {
    check_stack_canary(0); check_stack_canary(1); check_stack_canary(2); check_stack_canary(3);
    __atomic_store_n((int*)&done_count, 0, __ATOMIC_RELAXED);
    asm volatile("dsb ish" : : : "memory");
    __atomic_fetch_add((int*)&task_epoch, 1, __ATOMIC_RELEASE);
    asm volatile("sev");
}

void parallel_conv2d_async_start(const float* in, int H_in, int W_in, int C_in,
                                 const float* w_rep, const float* bias, int C_out, int K,
                                 int stride, int pad, bool do_silu, float* out) {
    int grp_w = (C_out >= 32 && (C_out & 7) == 0) ? 8 : 4;
    int n_grp = C_out / grp_w;
    probe_multicore();
    if (!multicore_available) {
        if (grp_w == 8) conv2d_partial_8ch(in, H_in, W_in, C_in, w_rep, bias, K, stride, pad, do_silu, out, 0, n_grp);
        else            conv2d_partial(in, H_in, W_in, C_in, w_rep, bias, K, stride, pad, do_silu, out, 0, n_grp);
        /* Single-core fallback: result is ready immediately. Mark done so
         * parallel_async_done() returns true on the first poll. */
        __atomic_store_n((int*)&done_count, 3, __ATOMIC_RELEASE);
        return;
    }
    parallel_task.type = TASK_CONV2D_ASYNC; parallel_task.in = in;
    parallel_task.H = H_in; parallel_task.W = W_in;
    parallel_task.C_in = C_in; parallel_task.C_out = C_out;
    parallel_task.w_rep = w_rep; parallel_task.bias = bias;
    parallel_task.K = K; parallel_task.stride = stride; parallel_task.pad = pad;
    parallel_task.do_silu = do_silu;
    parallel_task.out = out; parallel_task.n_grp = n_grp;
    dispatch_task_async();
}

void parallel_conv1x1_async_start(const float* in, int H, int W, int C_in,
                                  const float* w, const float* b, int C_out,
                                  bool do_silu, float* out) {
    probe_multicore();
    if (!multicore_available) {
        ops_neon_conv1x1_kernel(in, H, W, C_in, w, b, 0, C_out, C_out, do_silu, out);
        __atomic_store_n((int*)&done_count, 3, __ATOMIC_RELEASE);
        return;
    }
    parallel_task.type = TASK_CONV1X1_ASYNC; parallel_task.in = in;
    parallel_task.H = H; parallel_task.W = W;
    parallel_task.C_in = C_in; parallel_task.C_out = C_out;
    parallel_task.w1x1 = w; parallel_task.bias = b;
    parallel_task.do_silu = do_silu; parallel_task.out = out;
    dispatch_task_async();
}

bool parallel_async_done() {
    return __atomic_load_n((int*)&done_count, __ATOMIC_ACQUIRE) >= 3;
}

void parallel_async_wait() {
    if (!multicore_available) return;
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
