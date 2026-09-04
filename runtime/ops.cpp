/* File: src/ops.cpp - FUSED OPERATOR KERNELS & POOLING */
#include "ops.h"
#include <arm_neon.h>
#include <stdint.h>

#ifndef NULL
#define NULL 0
#endif

extern "C" {

static inline float mini_expf(float x) {
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

static inline float mini_sigmoidf(float x) {
    return 1.0f / (1.0f + mini_expf(-x));
}

static inline float32x4_t neon_expf4(float32x4_t x) {
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

static inline float32x4_t neon_silu(float32x4_t x) {
    float32x4_t e = neon_expf4(vnegq_f32(x));
    float32x4_t denom = vaddq_f32(e, vdupq_n_f32(1.0f));
    float32x4_t recip = vrecpeq_f32(denom);
    recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
    recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
    return vmulq_f32(x, recip);
}

/* conv1x1 kernel — out[co][p] = b[co] + Σ_ci w[co][ci] · in[ci][p].
 *
 * V190 (2026-09-04): the input is [C_in][HW] and the old inner loop did
 * `vld1q_f32(in + ci*HW + p)` for consecutive ci — HW*4 bytes apart. When HW
 * is a multiple of 2048 (YOLO_IN=256 → S4²=4096) every ci maps to the SAME
 * 32 KB / 4-way L1 set, so C_in loads = C_in−4 conflict misses, repeated for
 * every output-channel group. L2's cv2 (48→32 @64²) was 55 ms this way —
 * ~18× its compute floor.
 *
 * Fix: transpose each 4-position group of the input into `xt` ([C_in][4],
 * contiguous over ci, ~L1-resident) ONCE, then every co-group reads it
 * sequentially. Strided reads drop from (C_out/4)·C_in to C_in per group.
 * The 8-output-channel body (B2, 2026-05-11) is kept. C_in > C1X1_MAX_CIN
 * (never in this model — SPPF cv2 is the max at 512) uses a plain path. */
#define TILE_P       32
#define C1X1_MAX_CIN 512

static void conv1x1_slow(const float* in, int HW, int C_in, const float* w, const float* b,
                         int C_out_start, int C_out_end, bool do_silu, float* out) {
    for (int co = C_out_start; co < C_out_end; co++) {
        const float* wr = w + (long)co * C_in;
        float* och = out + (long)co * HW;
        for (int p = 0; p < HW; p++) {
            float acc = b ? b[co] : 0.0f;
            for (int ci = 0; ci < C_in; ci++) acc += wr[ci] * in[(long)ci * HW + p];
            if (do_silu) acc *= mini_sigmoidf(acc);
            och[p] = acc;
        }
    }
}

void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in, const float* w, const float* b, int C_out_start, int C_out_end, int C_out_total, bool do_silu, float* out) {
    (void)C_out_total;
    int HW = H * W;

    if (C_in > C1X1_MAX_CIN) {   /* never in this model (SPPF cv2 = 512 is the max) */
        conv1x1_slow(in, HW, C_in, w, b, C_out_start, C_out_end, do_silu, out);
        return;
    }

    for (int p_step = 0; p_step < HW; p_step += TILE_P) {
        int p_max = (p_step + TILE_P < HW) ? p_step + TILE_P : HW;
        int p_vec = p_step + ((p_max - p_step) & ~3);   /* last 4-aligned p */

        for (int p = p_step; p < p_vec; p += 4) {
            /* Transpose in[*][p..p+3] → xt[ci*4 .. ci*4+3], contiguous over ci. */
            float xt[C1X1_MAX_CIN * 4];
            for (int ci = 0; ci < C_in; ci++) {
                if (ci + 1 < C_in) __builtin_prefetch(in + (long)(ci + 1) * HW + p, 0, 3);
                vst1q_f32(xt + ci * 4, vld1q_f32(in + (long)ci * HW + p));
            }

            int co = C_out_start;
            for (; co <= C_out_end - 8; co += 8) {
                const float* w0 = w + (long)(co+0)*C_in; const float* w1 = w + (long)(co+1)*C_in;
                const float* w2 = w + (long)(co+2)*C_in; const float* w3 = w + (long)(co+3)*C_in;
                const float* w4 = w + (long)(co+4)*C_in; const float* w5 = w + (long)(co+5)*C_in;
                const float* w6 = w + (long)(co+6)*C_in; const float* w7 = w + (long)(co+7)*C_in;
                float32x4_t a0 = vdupq_n_f32(b?b[co+0]:0.0f), a1 = vdupq_n_f32(b?b[co+1]:0.0f);
                float32x4_t a2 = vdupq_n_f32(b?b[co+2]:0.0f), a3 = vdupq_n_f32(b?b[co+3]:0.0f);
                float32x4_t a4 = vdupq_n_f32(b?b[co+4]:0.0f), a5 = vdupq_n_f32(b?b[co+5]:0.0f);
                float32x4_t a6 = vdupq_n_f32(b?b[co+6]:0.0f), a7 = vdupq_n_f32(b?b[co+7]:0.0f);
                for (int ci = 0; ci < C_in; ci++) {
                    float32x4_t v = vld1q_f32(xt + ci * 4);
                    a0 = vmlaq_n_f32(a0, v, w0[ci]); a1 = vmlaq_n_f32(a1, v, w1[ci]);
                    a2 = vmlaq_n_f32(a2, v, w2[ci]); a3 = vmlaq_n_f32(a3, v, w3[ci]);
                    a4 = vmlaq_n_f32(a4, v, w4[ci]); a5 = vmlaq_n_f32(a5, v, w5[ci]);
                    a6 = vmlaq_n_f32(a6, v, w6[ci]); a7 = vmlaq_n_f32(a7, v, w7[ci]);
                }
                if (do_silu) {
                    a0 = neon_silu(a0); a1 = neon_silu(a1); a2 = neon_silu(a2); a3 = neon_silu(a3);
                    a4 = neon_silu(a4); a5 = neon_silu(a5); a6 = neon_silu(a6); a7 = neon_silu(a7);
                }
                vst1q_f32(out + (long)(co+0)*HW + p, a0); vst1q_f32(out + (long)(co+1)*HW + p, a1);
                vst1q_f32(out + (long)(co+2)*HW + p, a2); vst1q_f32(out + (long)(co+3)*HW + p, a3);
                vst1q_f32(out + (long)(co+4)*HW + p, a4); vst1q_f32(out + (long)(co+5)*HW + p, a5);
                vst1q_f32(out + (long)(co+6)*HW + p, a6); vst1q_f32(out + (long)(co+7)*HW + p, a7);
            }
            for (; co <= C_out_end - 4; co += 4) {
                const float* w0 = w + (long)(co+0)*C_in; const float* w1 = w + (long)(co+1)*C_in;
                const float* w2 = w + (long)(co+2)*C_in; const float* w3 = w + (long)(co+3)*C_in;
                float32x4_t a0 = vdupq_n_f32(b?b[co+0]:0.0f), a1 = vdupq_n_f32(b?b[co+1]:0.0f);
                float32x4_t a2 = vdupq_n_f32(b?b[co+2]:0.0f), a3 = vdupq_n_f32(b?b[co+3]:0.0f);
                for (int ci = 0; ci < C_in; ci++) {
                    float32x4_t v = vld1q_f32(xt + ci * 4);
                    a0 = vmlaq_n_f32(a0, v, w0[ci]); a1 = vmlaq_n_f32(a1, v, w1[ci]);
                    a2 = vmlaq_n_f32(a2, v, w2[ci]); a3 = vmlaq_n_f32(a3, v, w3[ci]);
                }
                if (do_silu) { a0 = neon_silu(a0); a1 = neon_silu(a1); a2 = neon_silu(a2); a3 = neon_silu(a3); }
                vst1q_f32(out + (long)(co+0)*HW + p, a0); vst1q_f32(out + (long)(co+1)*HW + p, a1);
                vst1q_f32(out + (long)(co+2)*HW + p, a2); vst1q_f32(out + (long)(co+3)*HW + p, a3);
            }
            for (; co < C_out_end; co++) {
                const float* wr = w + (long)co * C_in;
                float32x4_t a = vdupq_n_f32(b ? b[co] : 0.0f);
                for (int ci = 0; ci < C_in; ci++) a = vmlaq_n_f32(a, vld1q_f32(xt + ci * 4), wr[ci]);
                if (do_silu) a = neon_silu(a);
                vst1q_f32(out + (long)co * HW + p, a);
            }
        }

        /* Ragged tail: p_max−p_step not a multiple of 4. Never hit for
         * YOLO_IN=256 (every HW is a multiple of 32) — kept for safety. */
        for (int p = p_vec; p < p_max; p++) {
            for (int co = C_out_start; co < C_out_end; co++) {
                const float* wr = w + (long)co * C_in;
                float acc = b ? b[co] : 0.0f;
                for (int ci = 0; ci < C_in; ci++) acc += wr[ci] * in[(long)ci * HW + p];
                if (do_silu) acc *= mini_sigmoidf(acc);
                out[(long)co * HW + p] = acc;
            }
        }
    }
}

} // FIN DE EXTERN "C"

// --- POOLING / RESAMPLING (Fuera de extern "C") ---
void upsample2x_nearest(const float* in, float* out, int H, int W, int C) {
    for (int c = 0; c < C; c++) {
        const float* in_ch = in + c * H * W;
        float* out_ch = out + c * (H * 2) * (W * 2);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float val = in_ch[y * W + x];
                out_ch[(y*2)*(W*2)+(x*2)] = val;
                out_ch[(y*2)*(W*2)+(x*2+1)] = val;
                out_ch[(y*2+1)*(W*2)+(x*2)] = val;
                out_ch[(y*2+1)*(W*2)+(x*2+1)] = val;
            }
        }
    }
}

/* P2: NEON-accelerated maxpool5x5 stride=1 pad=2.
 * Interior pixels (no padding needed) use vmaxq_f32 to process 4 of the 5
 * kernel values per row simultaneously, then reduce horizontally. */
void maxpool5x5_s1_p2(const float* in, float* out, int H, int W, int C) {
    for (int c = 0; c < C; c++) {
        const float* in_ch = in + c * H * W;
        float* out_ch = out + c * H * W;
        for (int oy = 0; oy < H; oy++) {
            for (int ox = 0; ox < W; ox++) {
                /* Interior: rows [2, H-3], cols [2, W-3] — no border clamping */
                if (oy >= 2 && oy <= H - 3 && ox >= 2 && ox <= W - 3) {
                    float32x4_t vmax = vdupq_n_f32(-3.40282347e+38f);
                    float scalar_max = -3.40282347e+38f;
                    for (int ky = -2; ky <= 2; ky++) {
                        const float* row = in_ch + (oy + ky) * W + (ox - 2);
                        /* Load 4 of the 5 window values with NEON */
                        vmax = vmaxq_f32(vmax, vld1q_f32(row));
                        /* 5th value scalar */
                        float v4 = row[4];
                        if (v4 > scalar_max) scalar_max = v4;
                    }
                    /* Horizontal reduce NEON max */
                    float32x2_t v2 = vpmax_f32(vget_low_f32(vmax), vget_high_f32(vmax));
                    v2 = vpmax_f32(v2, v2);
                    float neon_max = vget_lane_f32(v2, 0);
                    out_ch[oy * W + ox] = (neon_max > scalar_max) ? neon_max : scalar_max;
                } else {
                    /* Border: scalar fallback with clamp */
                    float max_val = -3.40282347e+38f;
                    for (int ky = -2; ky <= 2; ky++) {
                        int iy = oy + ky;
                        if (iy < 0 || iy >= H) continue;
                        for (int kx = -2; kx <= 2; kx++) {
                            int ix = ox + kx;
                            if (ix < 0 || ix >= W) continue;
                            float val = in_ch[iy * W + ix];
                            if (val > max_val) max_val = val;
                        }
                    }
                    out_ch[oy * W + ox] = max_val;
                }
            }
        }
    }
}
