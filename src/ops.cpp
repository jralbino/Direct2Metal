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

/* conv1x1 kernel: p_step-outer (cache-friendly — input tile loaded once, reused
 * across all output channel groups). Bias recomputed per tile (negligible cost). */
void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in, const float* w, const float* b, int C_out_start, int C_out_end, int C_out_total, bool do_silu, float* out) {
    int HW = H * W;
    #define TILE_P 32

    for (int p_step = 0; p_step < HW; p_step += TILE_P) {
        int p_max = (p_step + TILE_P < HW) ? p_step + TILE_P : HW;
        int co = C_out_start;
        for (; co <= C_out_end - 4; co += 4) {
            /* bias computed once per (tile, co-group): 4 cheap vdup vs re-reading in[] */
            float32x4_t bias0 = vdupq_n_f32(b ? b[co+0] : 0.0f);
            float32x4_t bias1 = vdupq_n_f32(b ? b[co+1] : 0.0f);
            float32x4_t bias2 = vdupq_n_f32(b ? b[co+2] : 0.0f);
            float32x4_t bias3 = vdupq_n_f32(b ? b[co+3] : 0.0f);
            const float* w0 = w + (co+0) * C_in;
            const float* w1 = w + (co+1) * C_in;
            const float* w2 = w + (co+2) * C_in;
            const float* w3 = w + (co+3) * C_in;
            float* out0 = out + (co+0) * HW;
            float* out1 = out + (co+1) * HW;
            float* out2 = out + (co+2) * HW;
            float* out3 = out + (co+3) * HW;
            int p = p_step;
            for (; p <= p_max - 4; p += 4) {
                float32x4_t acc0 = bias0, acc1 = bias1, acc2 = bias2, acc3 = bias3;
                for (int ci = 0; ci < C_in; ci++) {
                    float32x4_t v_in = vld1q_f32(in + ci * HW + p);
                    acc0 = vmlaq_f32(acc0, v_in, vdupq_n_f32(w0[ci]));
                    acc1 = vmlaq_f32(acc1, v_in, vdupq_n_f32(w1[ci]));
                    acc2 = vmlaq_f32(acc2, v_in, vdupq_n_f32(w2[ci]));
                    acc3 = vmlaq_f32(acc3, v_in, vdupq_n_f32(w3[ci]));
                }
                if (do_silu) {
                    acc0 = neon_silu(acc0); acc1 = neon_silu(acc1);
                    acc2 = neon_silu(acc2); acc3 = neon_silu(acc3);
                }
                vst1q_f32(out0+p, acc0); vst1q_f32(out1+p, acc1);
                vst1q_f32(out2+p, acc2); vst1q_f32(out3+p, acc3);
            }
            for (; p < p_max; p++) {
                float s0=b?b[co+0]:0.0f, s1=b?b[co+1]:0.0f, s2=b?b[co+2]:0.0f, s3=b?b[co+3]:0.0f;
                for (int ci = 0; ci < C_in; ci++) {
                    float v = in[ci*HW+p];
                    s0+=w0[ci]*v; s1+=w1[ci]*v; s2+=w2[ci]*v; s3+=w3[ci]*v;
                }
                if (do_silu) { s0*=mini_sigmoidf(s0); s1*=mini_sigmoidf(s1); s2*=mini_sigmoidf(s2); s3*=mini_sigmoidf(s3); }
                out0[p]=s0; out1[p]=s1; out2[p]=s2; out3[p]=s3;
            }
        }
        /* Scalar tail: remaining channels not divisible by 4 */
        for (; co < C_out_end; co++) {
            const float* wr = w + co * C_in;
            float* och = out + co * HW;
            for (int p = p_step; p < p_max; p++) {
                float acc = b ? b[co] : 0.0f;
                for (int ci = 0; ci < C_in; ci++) acc += wr[ci] * in[ci*HW+p];
                if (do_silu) acc *= mini_sigmoidf(acc);
                och[p] = acc;
            }
        }
    }
}

// RESTAURANDO KERNELS 3x3 Y FALLBACKS QUE NECESITA CONV2D.CPP
void conv2d_neon_4ch_group(const float* in, int H_in, int W_in, int C_in, const float* wg, int group_idx, int H_out, int W_out, int K, int stride, int pad, float* out) {
    int HW_out = H_out * W_out, HW_in = H_in * W_in;
    float *o0 = out + (group_idx*4+0)*HW_out, *o1 = out + (group_idx*4+1)*HW_out, *o2 = out + (group_idx*4+2)*HW_out, *o3 = out + (group_idx*4+3)*HW_out;
    for (int i = 0; i < HW_out; i++) { o0[i]=0; o1[i]=0; o2[i]=0; o3[i]=0; }

    for (int oy = 0; oy < H_out; oy++) {
        for (int ox = 0; ox < W_out; ox++) {
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (int ci = 0; ci < C_in; ci++) {
                const float* in_ch = in + ci * HW_in;
                const float* wci = wg + ci * K * K * 4;
                for (int ky = 0; ky < K; ky++) {
                    int iy = oy * stride + ky - pad;
                    if ((unsigned)iy >= (unsigned)H_in) continue;
                    for (int kx = 0; kx < K; kx++) {
                        int ix = ox * stride + kx - pad;
                        if ((unsigned)ix < (unsigned)W_in)
                            acc = vmlaq_n_f32(acc, vld1q_f32(wci + (ky*K+kx)*4), in_ch[iy * W_in + ix]);
                    }
                }
            }
            int pos = oy * W_out + ox;
            o0[pos] = vgetq_lane_f32(acc, 0); o1[pos] = vgetq_lane_f32(acc, 1);
            o2[pos] = vgetq_lane_f32(acc, 2); o3[pos] = vgetq_lane_f32(acc, 3);
        }
    }
}

void conv2d_cpp(const float* in, int H_in, int W_in, int C_in, const float* w, int C_out, int K, int stride, int pad, float* out) {
    int H_out = H_in / stride;
    int W_out = W_in / stride;
    int HW_out = H_out * W_out;
    for (int co = 0; co < C_out; co++) {
        float* och = out + co * HW_out;
        const float* w_filter = w + co * C_in * K * K;
        for (int i = 0; i < HW_out; i++) och[i] = 0.0f;
        for (int ci = 0; ci < C_in; ci++) {
            const float* in_ch = in + ci * H_in * W_in;
            const float* w_ch = w_filter + ci * K * K;
            for (int oy = 0; oy < H_out; oy++) {
                for (int ox = 0; ox < W_out; ox++) {
                    float acc = 0.0f;
                    for (int ky = 0; ky < K; ky++) {
                        int iy = oy * stride + ky - pad;
                        if (iy < 0 || iy >= H_in) continue;
                        for (int kx = 0; kx < K; kx++) {
                            int ix = ox * stride + kx - pad;
                            if (ix < 0 || ix >= W_in) continue;
                            acc += in_ch[iy * W_in + ix] * w_ch[ky * K + kx];
                        }
                    }
                    och[oy * W_out + ox] += acc;
                }
            }
        }
    }
}

} // FIN DE EXTERN "C"

// --- COMPATIBILITY WRAPPERS & POOLING (Fuera de extern "C") ---
void conv2d_neon_4ch(const float* in, int H_in, int W_in, int C_in, const float* w_rep, int C_out, int K, int stride, int pad, float* out) {
    int H_out = H_in / stride; int W_out = W_in / stride;
    for (int g = 0; g < C_out/4; g++) conv2d_neon_4ch_group(in, H_in, W_in, C_in, w_rep + g*C_in*K*K*4, g, H_out, W_out, K, stride, pad, out);
}

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
