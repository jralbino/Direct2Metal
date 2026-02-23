/* File: src/ops.cpp - THE ULTIMATE NEON KERNELS */
#include "ops.h"
#include <arm_neon.h>
#include <stdint.h>

#ifndef NULL
#define NULL 0
#endif

extern "C" {

static float mini_expf(float x) {
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

static inline float mini_sigmoidf(float x) { return 1.0f / (1.0f + mini_expf(-x)); }

// NEON vectorized exp: matches mini_expf algorithm
static inline float32x4_t neon_expf4(float32x4_t x) {
    x = vminq_f32(x, vdupq_n_f32(88.0f));
    x = vmaxq_f32(x, vdupq_n_f32(-88.0f));
    // z = x * log2(e), k = round(z)
    float32x4_t z = vmulq_n_f32(x, 1.44269504f);
    int32x4_t k = vcvtaq_s32_f32(z);
    // r = x - k * ln(2)
    float32x4_t r = vmlsq_n_f32(x, vcvtq_f32_s32(k), 0.69314718f);
    // polynomial: 1 + r*(1 + r*(0.5 + r*(1/6 + r*(1/24 + r/120))))
    float32x4_t p = vdupq_n_f32(0.00833333f);
    p = vmlaq_f32(vdupq_n_f32(0.04166667f), r, p);
    p = vmlaq_f32(vdupq_n_f32(0.16666667f), r, p);
    p = vmlaq_f32(vdupq_n_f32(0.5f), r, p);
    p = vmlaq_f32(vdupq_n_f32(1.0f), r, p);
    p = vmlaq_f32(vdupq_n_f32(1.0f), r, p);
    // scale by 2^k via float bit manipulation
    int32x4_t pow2 = vshlq_n_s32(vaddq_s32(k, vdupq_n_s32(127)), 23);
    return vmulq_f32(vreinterpretq_f32_s32(pow2), p);
}

void silu_inplace(float* buf, int n) {
    int i = 0;
    for (; i <= n - 4; i += 4) {
        float32x4_t x = vld1q_f32(buf + i);
        // sigmoid(x) = 1 / (1 + exp(-x))
        float32x4_t e = neon_expf4(vnegq_f32(x));
        float32x4_t denom = vaddq_f32(e, vdupq_n_f32(1.0f));
        // Newton-Raphson reciprocal: 2 iterations → ~23 bits accuracy
        float32x4_t recip = vrecpeq_f32(denom);
        recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
        recip = vmulq_f32(vrecpsq_f32(denom, recip), recip);
        vst1q_f32(buf + i, vmulq_f32(x, recip));
    }
    for (; i < n; i++) buf[i] *= mini_sigmoidf(buf[i]);
}

// --- FAST NEON 3x3 (Para pesos empaquetados) ---
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

// --- SUPER FAST NEON 1x1 (4x4 Register Blocking) ---
void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in, const float* w, const float* b, int C_out_start, int C_out_end, int C_out_total, float* out) {
    int HW = H * W;
    int co = C_out_start;
    for (; co <= C_out_end - 4; co += 4) {
        float32x4_t bias0 = vdupq_n_f32(b ? b[co+0] : 0.0f);
        float32x4_t bias1 = vdupq_n_f32(b ? b[co+1] : 0.0f);
        float32x4_t bias2 = vdupq_n_f32(b ? b[co+2] : 0.0f);
        float32x4_t bias3 = vdupq_n_f32(b ? b[co+3] : 0.0f);

        const float* w0 = w + (co+0) * C_in; const float* w1 = w + (co+1) * C_in;
        const float* w2 = w + (co+2) * C_in; const float* w3 = w + (co+3) * C_in;
        
        float* out0 = out + (co+0) * HW; float* out1 = out + (co+1) * HW;
        float* out2 = out + (co+2) * HW; float* out3 = out + (co+3) * HW;

        for (int p = 0; p <= HW - 4; p += 4) {
            float32x4_t acc0 = bias0, acc1 = bias1, acc2 = bias2, acc3 = bias3;
            for (int ci = 0; ci < C_in; ci++) {
                float32x4_t v_in = vld1q_f32(in + ci * HW + p);
                acc0 = vmlaq_f32(acc0, v_in, vdupq_n_f32(w0[ci])); acc1 = vmlaq_f32(acc1, v_in, vdupq_n_f32(w1[ci]));
                acc2 = vmlaq_f32(acc2, v_in, vdupq_n_f32(w2[ci])); acc3 = vmlaq_f32(acc3, v_in, vdupq_n_f32(w3[ci]));
            }
            vst1q_f32(out0 + p, acc0); vst1q_f32(out1 + p, acc1);
            vst1q_f32(out2 + p, acc2); vst1q_f32(out3 + p, acc3);
        }
        for (int p = (HW & ~3); p < HW; p++) {
            float sum0 = b?b[co]:0, sum1 = b?b[co+1]:0, sum2 = b?b[co+2]:0, sum3 = b?b[co+3]:0;
            for(int ci=0; ci<C_in; ci++) {
                float val = in[ci*HW+p];
                sum0 += val * w0[ci]; sum1 += val * w1[ci]; sum2 += val * w2[ci]; sum3 += val * w3[ci];
            }
            out0[p]=sum0; out1[p]=sum1; out2[p]=sum2; out3[p]=sum3;
        }
    }
    for (; co < C_out_end; co++) {
        const float* wr = w + co * C_in; float* och = out + co * HW; float bias = b ? b[co] : 0.0f;
        for (int p = 0; p < HW; p++) {
            float acc = bias; for (int ci = 0; ci < C_in; ci++) acc += wr[ci] * in[ci*HW+p]; och[p] = acc;
        }
    }
}

// --- FALLBACK C++ (Para la Capa 0) ---
void conv2d_cpp(const float* in, int H_in, int W_in, int C_in, const float* w, int C_out, int K, int stride, int pad, float* out) {
    int H_out = H_in / stride; int W_out = W_in / stride; int HW_out = H_out * W_out;
    for (int co = 0; co < C_out; co++) {
        float* och = out + co * HW_out; const float* w_filter = w + co * C_in * K * K;
        for (int i = 0; i < HW_out; i++) och[i] = 0.0f;
        for (int ci = 0; ci < C_in; ci++) {
            const float* in_ch = in + ci * H_in * W_in; const float* w_ch = w_filter + ci * K * K;
            for (int oy = 0; oy < H_out; oy++) {
                for (int ox = 0; ox < W_out; ox++) {
                    float acc = 0.0f;
                    for (int ky = 0; ky < K; ky++) {
                        int iy = oy * stride + ky - pad; if (iy < 0 || iy >= H_in) continue;
                        for (int kx = 0; kx < K; kx++) {
                            int ix = ox * stride + kx - pad; if (ix < 0 || ix >= W_in) continue;
                            acc += in_ch[iy * W_in + ix] * w_ch[ky * K + kx];
                        }
                    }
                    och[oy * W_out + ox] += acc;
                }
            }
        }
    }
}

} // FIN DEL BLOQUE EXTERN "C"

// --- COMPATIBILITY WRAPPERS & POOLING ---
void conv2d_neon_4ch(const float* in, int H_in, int W_in, int C_in, const float* w_rep, int C_out, int K, int stride, int pad, float* out) {
    int H_out = H_in / stride; int W_out = W_in / stride;
    for (int g = 0; g < C_out/4; g++) conv2d_neon_4ch_group(in, H_in, W_in, C_in, w_rep + g*C_in*K*K*4, g, H_out, W_out, K, stride, pad, out);
}
void upsample2x_nearest(const float* in, float* out, int H, int W, int C) {
    for (int c = 0; c < C; c++) {
        const float* in_ch = in + c * H * W; float* out_ch = out + c * (H * 2) * (W * 2);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float val = in_ch[y * W + x];
                out_ch[(y*2)*(W*2)+(x*2)] = val; out_ch[(y*2)*(W*2)+(x*2+1)] = val;
                out_ch[(y*2+1)*(W*2)+(x*2)] = val; out_ch[(y*2+1)*(W*2)+(x*2+1)] = val;
            }
        }
    }
}
void maxpool5x5_s1_p2(const float* in, float* out, int H, int W, int C) {
    for (int c = 0; c < C; c++) {
        const float* in_ch = in + c * H * W; float* out_ch = out + c * H * W;
        for (int oy = 0; oy < H; oy++) {
            for (int ox = 0; ox < W; ox++) {
                float max_val = -3.40282347e+38f; 
                for (int ky = -2; ky <= 2; ky++) {
                    int iy = oy + ky; if (iy < 0 || iy >= H) continue;
                    for (int kx = -2; kx <= 2; kx++) {
                        int ix = ox + kx; if (ix < 0 || ix >= W) continue;
                        float val = in_ch[iy * W + ix]; if (val > max_val) max_val = val;
                    }
                }
                out_ch[oy * W + ox] = max_val;
            }
        }
    }
}