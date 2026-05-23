/* File: runtime/ops_int8.cpp — INT8 (W8A8) NEON conv kernels.
 *
 * Session 2 of G2 Tier 2. Single-core reference kernels that mirror the
 * FP32 NEON ops layout exactly but operate on int8 inputs + int8 weights
 * + int32 accumulator + fp32 out-stage (multiply by scale_in*scale_w,
 * optional SiLU, quantize back to int8 with scale_out).
 *
 * Weight layout matches tools/calibrate_int8.py:
 *   - conv1x1: flat int8[C_out * C_in] (row-major, co-outer).
 *   - conv K×K (4ch): int8[C_out/4][C_in*K*K][4] — 4 weights per
 *     (ci,kh,kw) position, one per output channel in the group.
 *   - conv K×K (8ch): int8[C_out/8][C_in*K*K][8] — same but 8 co's per
 *     group, used when C_out >= 32 and divisible by 8.
 *
 * Bias is int32, pre-multiplied by 1/(scale_in*scale_w) so it adds
 * directly to the conv accumulator with no float ops in the hot loop.
 *
 * A53 is ARMv8.0 (no SDOT). The base int8 MLA path is
 *   vld1_s8 → vmovl_s8 → vmlal_s16
 * Each vmlal_s16 contributes 4 int32 accumulations per cycle pair.
 *
 * SiLU stays fp32 in the out-stage for this first cut. Session 3 will
 * add an int8 LUT path if profiling shows it dominates. */
#include "ops.h"
#include <arm_neon.h>
#include <stdint.h>


/* ── Local SiLU/exp helpers (mirror runtime/ops.cpp; static here to
 *    avoid touching the existing extern "C" block) ─────────────────────── */

static inline float mini_expf_i8(float x) {
    if (x > 88.0f) return 3.40282347e+38f;
    if (x < -88.0f) return 0.0f;
    float z = x * 1.44269504088896f;
    int32_t k = (z >= 0.0f) ? (int32_t)(z + 0.5f) : (int32_t)(z - 0.5f);
    float r = x - (float)k * 0.69314718055995f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.04166667f + r * 0.00833333f))));
    union { float f; uint32_t u; } bits;
    bits.u = (uint32_t)(k + 127) << 23;
    return bits.f * p;
}

static inline float32x4_t neon_expf4_i8(float32x4_t x) {
    x = vminq_f32(x, vdupq_n_f32(88.0f));
    x = vmaxq_f32(x, vdupq_n_f32(-88.0f));
    float32x4_t z = vmulq_n_f32(x, 1.44269504f);
    int32x4_t k = vcvtaq_s32_f32(z);
    float32x4_t r = vmlsq_n_f32(x, vcvtq_f32_s32(k), 0.69314718f);
    float32x4_t p = vdupq_n_f32(0.00833333f);
    p = vmlaq_f32(vdupq_n_f32(0.04166667f), r, p);
    p = vmlaq_f32(vdupq_n_f32(0.16666667f), r, p);
    p = vmlaq_f32(vdupq_n_f32(0.5f), r, p);
    p = vmlaq_f32(vdupq_n_f32(1.0f), r, p);
    p = vmlaq_f32(vdupq_n_f32(1.0f), r, p);
    int32x4_t pow2 = vshlq_n_s32(vaddq_s32(k, vdupq_n_s32(127)), 23);
    return vmulq_f32(vreinterpretq_f32_s32(pow2), p);
}

static inline float32x4_t neon_silu_i8(float32x4_t x) {
    float32x4_t e = neon_expf4_i8(vnegq_f32(x));
    float32x4_t denom = vaddq_f32(e, vdupq_n_f32(1.0f));
    float32x4_t recip = vrecpeq_f32(denom);
    recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
    recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
    return vmulq_f32(x, recip);
}

/* Saturate fp32x4 → int8 (low 4 lanes of returned int8x8). */
static inline int8x8_t quant_f32_to_s8_low(float32x4_t f) {
    int32x4_t i32 = vcvtnq_s32_f32(f);
    int16x4_t i16 = vqmovn_s32(i32);
    int16x8_t i16_full = vcombine_s16(i16, i16);
    return vqmovn_s16(i16_full);
}

/* Saturate two fp32x4 → int8x8 (8 contiguous bytes). */
static inline int8x8_t quant_two_f32_to_s8(float32x4_t f_lo, float32x4_t f_hi) {
    int32x4_t q32_lo = vcvtnq_s32_f32(f_lo);
    int32x4_t q32_hi = vcvtnq_s32_f32(f_hi);
    int16x4_t q16_lo = vqmovn_s32(q32_lo);
    int16x8_t q16    = vqmovn_high_s32(q16_lo, q32_hi);  /* [lo low4, hi low4] */
    return vqmovn_s16(q16);
}

/* Scalar SiLU + per-channel quantize. Used by scalar tails of the kernels. */
static inline int8_t silu_quant_scalar(int32_t acc, float sw_si, float inv_so, bool do_silu) {
    float f = (float)acc * sw_si;
    if (do_silu) {
        float sig = 1.0f / (1.0f + mini_expf_i8(-f));
        f = f * sig;
    }
    f = f * inv_so;
    int32_t q = (int32_t)(f + (f >= 0 ? 0.5f : -0.5f));
    if (q > 127) q = 127;
    if (q < -128) q = -128;
    return (int8_t)q;
}


extern "C" {

/* ────────────────────────────────────────────────────────────────────
 * conv1x1_int8 — int8 conv with K=1.
 *
 * Weight layout: flat int8[C_out][C_in]. For each output channel, a row
 * of C_in int8 weights. Bias is int32[C_out].
 *
 * NEON: vectorize over 8 contiguous input positions per output channel.
 * For each position group of 8, load v_in = vld1_s8(in + ci*HW + p) and
 * broadcast the int8 weight scalar w[co][ci] as int16x4. vmlal_s16
 * accumulates 4 multiplies per call into the int32 accumulator.
 *
 * (Multi-core dispatch + 8-co tiling come in Session 3.) ─────────────── */
void conv1x1_int8(const int8_t* in, int H, int W, int C_in,
                  const int8_t* w, const int32_t* bias,
                  float scale_w, float scale_in, float scale_out,
                  int C_out, bool do_silu,
                  int8_t* out) {
    int HW = H * W;
    float sw_si  = scale_w * scale_in;
    float inv_so = 1.0f / scale_out;

    for (int co = 0; co < C_out; co++) {
        const int8_t* wr   = w   + co * C_in;
        int8_t*       och  = out + co * HW;
        int32_t bias_co    = bias ? bias[co] : 0;
        int32x4_t bias_v   = vdupq_n_s32(bias_co);

        int p = 0;
        for (; p <= HW - 8; p += 8) {
            int32x4_t acc_lo = bias_v;
            int32x4_t acc_hi = bias_v;

            for (int ci = 0; ci < C_in; ci++) {
                if (ci + 1 < C_in)
                    __builtin_prefetch(in + (ci + 1) * HW + p, 0, 3);
                int8x8_t  v_in    = vld1_s8(in + ci * HW + p);
                int16x8_t v_in16  = vmovl_s8(v_in);
                int16x4_t w_dup   = vdup_n_s16((int16_t)wr[ci]);
                acc_lo = vmlal_s16(acc_lo, vget_low_s16(v_in16),  w_dup);
                acc_hi = vmlal_s16(acc_hi, vget_high_s16(v_in16), w_dup);
            }

            float32x4_t f_lo = vmulq_n_f32(vcvtq_f32_s32(acc_lo), sw_si);
            float32x4_t f_hi = vmulq_n_f32(vcvtq_f32_s32(acc_hi), sw_si);
            if (do_silu) {
                f_lo = neon_silu_i8(f_lo);
                f_hi = neon_silu_i8(f_hi);
            }
            f_lo = vmulq_n_f32(f_lo, inv_so);
            f_hi = vmulq_n_f32(f_hi, inv_so);

            int8x8_t q8 = quant_two_f32_to_s8(f_lo, f_hi);
            vst1_s8(och + p, q8);
        }
        /* Scalar tail. HW is always a multiple of 8 in YOLOv8n@256, so
         * this rarely runs — keep it for safety / non-standard sizes. */
        for (; p < HW; p++) {
            int32_t acc = bias_co;
            for (int ci = 0; ci < C_in; ci++) {
                acc += (int32_t)wr[ci] * (int32_t)in[ci * HW + p];
            }
            och[p] = silu_quant_scalar(acc, sw_si, inv_so, do_silu);
        }
    }
}


/* ────────────────────────────────────────────────────────────────────
 * conv2d_neon_4ch_group_int8 — one group of 4 output channels.
 *
 * Mirrors conv2d_neon_4ch_group (FP32) but uses int8 weights and a
 * single int32x4 accumulator across the 4 co's in the group.
 *
 * Per output position (oy, ox), per (ci, ky, kx):
 *   load 4 int8 weights for the 4 co's at this kernel position,
 *   broadcast the scalar int8 input from (ci, iy, ix),
 *   vmlal_s16 → 4 int32 accumulations in parallel.
 *
 * Padding handled by the `(unsigned)iy >= H_in` trick (negative iy
 * wraps to huge unsigned > H_in, falls through the guard). ──────────── */
static void conv2d_neon_4ch_group_int8(const int8_t* in, int H_in, int W_in, int C_in,
                                       const int8_t* wg, int group_idx,
                                       int H_out, int W_out, int K, int stride, int pad,
                                       float scale_w, float scale_in, float scale_out,
                                       const int32_t* bias, bool do_silu,
                                       int8_t* out) {
    int HW_out = H_out * W_out, HW_in = H_in * W_in;
    int8_t* o0 = out + (group_idx*4 + 0) * HW_out;
    int8_t* o1 = out + (group_idx*4 + 1) * HW_out;
    int8_t* o2 = out + (group_idx*4 + 2) * HW_out;
    int8_t* o3 = out + (group_idx*4 + 3) * HW_out;

    float sw_si  = scale_w * scale_in;
    float inv_so = 1.0f / scale_out;
    int32x4_t bias_v = bias ? vld1q_s32(bias + group_idx * 4)
                            : vdupq_n_s32(0);

    for (int oy = 0; oy < H_out; oy++) {
        for (int ox = 0; ox < W_out; ox++) {
            int32x4_t acc = bias_v;

            for (int ci = 0; ci < C_in; ci++) {
                const int8_t* in_ch = in + ci * HW_in;
                const int8_t* wci   = wg + ci * K * K * 4;

                for (int ky = 0; ky < K; ky++) {
                    int iy = oy * stride + ky - pad;
                    if ((unsigned)iy >= (unsigned)H_in) continue;

                    for (int kx = 0; kx < K; kx++) {
                        int ix = ox * stride + kx - pad;
                        if ((unsigned)ix < (unsigned)W_in) {
                            /* 4 weights for this (ci,ky,kx) across 4 co's */
                            int8x8_t  w8  = vreinterpret_s8_u32(
                                vld1_dup_u32((const uint32_t*)(wci + (ky*K + kx) * 4)));
                            int16x4_t w16 = vget_low_s16(vmovl_s8(w8));

                            /* Broadcast scalar int8 input → int16x4 */
                            int16x4_t in_dup = vdup_n_s16((int16_t)in_ch[iy * W_in + ix]);

                            acc = vmlal_s16(acc, w16, in_dup);
                        }
                    }
                }
            }

            float32x4_t f = vmulq_n_f32(vcvtq_f32_s32(acc), sw_si);
            if (do_silu) f = neon_silu_i8(f);
            f = vmulq_n_f32(f, inv_so);

            int8x8_t q8 = quant_f32_to_s8_low(f);
            int pos = oy * W_out + ox;
            o0[pos] = vget_lane_s8(q8, 0);
            o1[pos] = vget_lane_s8(q8, 1);
            o2[pos] = vget_lane_s8(q8, 2);
            o3[pos] = vget_lane_s8(q8, 3);
        }
    }
}

void conv2d_neon_4ch_int8(const int8_t* in, int H_in, int W_in, int C_in,
                          const int8_t* w_rep, int C_out, int K, int stride, int pad,
                          float scale_w, float scale_in, float scale_out,
                          const int32_t* bias, bool do_silu,
                          int8_t* out) {
    int H_out = H_in / stride;
    int W_out = W_in / stride;
    for (int g = 0; g < C_out / 4; g++) {
        conv2d_neon_4ch_group_int8(in, H_in, W_in, C_in,
                                   w_rep + g * C_in * K * K * 4, g,
                                   H_out, W_out, K, stride, pad,
                                   scale_w, scale_in, scale_out, bias, do_silu, out);
    }
}


/* ────────────────────────────────────────────────────────────────────
 * conv2d_neon_8ch_group_int8 — 8 output channels per group.
 *
 * Same shape as the 4-channel variant but each (ci,ky,kx) position
 * yields 8 int8 weights. Two int32x4 accumulators (lo for co 0-3,
 * hi for co 4-7). Layout is int8[C_out/8][C_in*K*K][8]. ──────────────── */
static void conv2d_neon_8ch_group_int8(const int8_t* in, int H_in, int W_in, int C_in,
                                       const int8_t* wg, int group_idx,
                                       int H_out, int W_out, int K, int stride, int pad,
                                       float scale_w, float scale_in, float scale_out,
                                       const int32_t* bias, bool do_silu,
                                       int8_t* out) {
    int HW_out = H_out * W_out, HW_in = H_in * W_in;
    int co_base = group_idx * 8;
    int8_t* o0 = out + (co_base + 0) * HW_out;
    int8_t* o1 = out + (co_base + 1) * HW_out;
    int8_t* o2 = out + (co_base + 2) * HW_out;
    int8_t* o3 = out + (co_base + 3) * HW_out;
    int8_t* o4 = out + (co_base + 4) * HW_out;
    int8_t* o5 = out + (co_base + 5) * HW_out;
    int8_t* o6 = out + (co_base + 6) * HW_out;
    int8_t* o7 = out + (co_base + 7) * HW_out;

    float sw_si  = scale_w * scale_in;
    float inv_so = 1.0f / scale_out;
    int32x4_t bias_lo = bias ? vld1q_s32(bias + co_base + 0) : vdupq_n_s32(0);
    int32x4_t bias_hi = bias ? vld1q_s32(bias + co_base + 4) : vdupq_n_s32(0);

    for (int oy = 0; oy < H_out; oy++) {
        for (int ox = 0; ox < W_out; ox++) {
            int32x4_t acc_lo = bias_lo;
            int32x4_t acc_hi = bias_hi;

            for (int ci = 0; ci < C_in; ci++) {
                const int8_t* in_ch = in + ci * HW_in;
                const int8_t* wci   = wg + ci * K * K * 8;

                for (int ky = 0; ky < K; ky++) {
                    int iy = oy * stride + ky - pad;
                    if ((unsigned)iy >= (unsigned)H_in) continue;

                    for (int kx = 0; kx < K; kx++) {
                        int ix = ox * stride + kx - pad;
                        if ((unsigned)ix < (unsigned)W_in) {
                            int8x8_t  w8    = vld1_s8(wci + (ky*K + kx) * 8);
                            int16x8_t w16   = vmovl_s8(w8);
                            int16x4_t in_dup = vdup_n_s16((int16_t)in_ch[iy * W_in + ix]);
                            acc_lo = vmlal_s16(acc_lo, vget_low_s16(w16),  in_dup);
                            acc_hi = vmlal_s16(acc_hi, vget_high_s16(w16), in_dup);
                        }
                    }
                }
            }

            float32x4_t f_lo = vmulq_n_f32(vcvtq_f32_s32(acc_lo), sw_si);
            float32x4_t f_hi = vmulq_n_f32(vcvtq_f32_s32(acc_hi), sw_si);
            if (do_silu) {
                f_lo = neon_silu_i8(f_lo);
                f_hi = neon_silu_i8(f_hi);
            }
            f_lo = vmulq_n_f32(f_lo, inv_so);
            f_hi = vmulq_n_f32(f_hi, inv_so);

            int8x8_t q8 = quant_two_f32_to_s8(f_lo, f_hi);
            int pos = oy * W_out + ox;
            o0[pos] = vget_lane_s8(q8, 0);
            o1[pos] = vget_lane_s8(q8, 1);
            o2[pos] = vget_lane_s8(q8, 2);
            o3[pos] = vget_lane_s8(q8, 3);
            o4[pos] = vget_lane_s8(q8, 4);
            o5[pos] = vget_lane_s8(q8, 5);
            o6[pos] = vget_lane_s8(q8, 6);
            o7[pos] = vget_lane_s8(q8, 7);
        }
    }
}

void conv2d_neon_8ch_int8(const int8_t* in, int H_in, int W_in, int C_in,
                          const int8_t* w_rep, int C_out, int K, int stride, int pad,
                          float scale_w, float scale_in, float scale_out,
                          const int32_t* bias, bool do_silu,
                          int8_t* out) {
    int H_out = H_in / stride;
    int W_out = W_in / stride;
    for (int g = 0; g < C_out / 8; g++) {
        conv2d_neon_8ch_group_int8(in, H_in, W_in, C_in,
                                   w_rep + g * C_in * K * K * 8, g,
                                   H_out, W_out, K, stride, pad,
                                   scale_w, scale_in, scale_out, bias, do_silu, out);
    }
}

} /* extern "C" */
