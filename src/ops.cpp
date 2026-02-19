/* File: src/ops.cpp — Bare-metal neural network operators.
 *
 * Designed for -ffreestanding -nostdlib.
 * Uses arm_neon.h (compiler built-in, available without libc).
 */

#include "ops.h"
#include <stdint.h>
#include <arm_neon.h>

/* ================================================================
 * Internal math (no libm)
 * ================================================================ */

static float mini_expf(float x) {
    if (x >  88.0f) return 3.40282347e+38f;
    if (x < -88.0f) return 0.0f;
    const float log2e = 1.44269504088896f;
    const float ln2   = 0.69314718055995f;
    float z = x * log2e;
    int32_t k = (z >= 0.0f) ? (int32_t)(z + 0.5f) : (int32_t)(z - 0.5f);
    float r = x - (float)k * ln2;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f
              + r * (0.04166667f + r * 0.00833333f))));
    union { float f; uint32_t u; } bits;
    bits.u = (uint32_t)(k + 127) << 23;
    return bits.f * p;
}

static inline float mini_sigmoidf(float x) {
    return 1.0f / (1.0f + mini_expf(-x));
}

static inline float fmaxf2(float a, float b) { return a > b ? a : b; }
static inline float fminf2(float a, float b) { return a < b ? a : b; }

/* ================================================================
 * batchnorm_inplace  (NEON vectorised)
 *
 * buf[C][HW]:  buf[c*HW + i] = scale[c] * buf[c*HW + i] + bias[c]
 * ================================================================ */
void batchnorm_inplace(float* buf, int HW, int C,
                       const float* scale, const float* bias) {
    for (int c = 0; c < C; c++) {
        float* ch = buf + c * HW;
        float32x4_t vs = vdupq_n_f32(scale[c]);
        float32x4_t vb = vdupq_n_f32(bias[c]);
        int i = 0;
        /* Process 4 floats per iteration */
        for (; i <= HW - 4; i += 4) {
            float32x4_t v = vld1q_f32(ch + i);
            v = vmlaq_f32(vb, v, vs);   /* v = bias + v * scale */
            vst1q_f32(ch + i, v);
        }
        /* Scalar tail */
        for (; i < HW; i++) {
            ch[i] = scale[c] * ch[i] + bias[c];
        }
    }
}

/* ================================================================
 * leaky_relu_inplace  (NEON vectorised)
 * ================================================================ */
void leaky_relu_inplace(float* buf, int n) {
    float32x4_t zero  = vdupq_n_f32(0.0f);
    float32x4_t alpha = vdupq_n_f32(0.1f);
    int i = 0;
    for (; i <= n - 4; i += 4) {
        float32x4_t v    = vld1q_f32(buf + i);
        float32x4_t neg  = vmulq_f32(v, alpha);        /* 0.1 * v */
        uint32x4_t  mask = vcltq_f32(v, zero);         /* v < 0 ? */
        v = vbslq_f32(mask, neg, v);                   /* select  */
        vst1q_f32(buf + i, v);
    }
    for (; i < n; i++) {
        if (buf[i] < 0.0f) buf[i] *= 0.1f;
    }
}

/* ================================================================
 * maxpool2x2  (CHW, stride=2)
 * in[C][H][W]  →  out[C][H/2][W/2]
 * ================================================================ */
void maxpool2x2(const float* in, float* out, int H, int W, int C) {
    int H2 = H / 2, W2 = W / 2;
    for (int c = 0; c < C; c++) {
        const float* in_ch  = in  + c * H  * W;
        float*       out_ch = out + c * H2 * W2;
        for (int oy = 0; oy < H2; oy++) {
            for (int ox = 0; ox < W2; ox++) {
                int iy = oy * 2, ix = ox * 2;
                float v00 = in_ch[ iy    * W + ix    ];
                float v01 = in_ch[ iy    * W + ix + 1];
                float v10 = in_ch[(iy+1) * W + ix    ];
                float v11 = in_ch[(iy+1) * W + ix + 1];
                out_ch[oy * W2 + ox] = fmaxf2(fmaxf2(v00, v01),
                                              fmaxf2(v10, v11));
            }
        }
    }
}

/* ================================================================
 * conv2d_cpp — scalar reference (kept for fallback / validation)
 * ================================================================ */
void conv2d_cpp(const float* in, int H_in, int W_in, int C_in,
                const float* w, int C_out, int K, int stride, int pad,
                float* out) {
    int H_out = (H_in + 2 * pad - K) / stride + 1;
    int W_out = (W_in + 2 * pad - K) / stride + 1;
    int out_sz = C_out * H_out * W_out;
    for (int i = 0; i < out_sz; i++) out[i] = 0.0f;

    for (int co = 0; co < C_out; co++) {
        for (int oy = 0; oy < H_out; oy++) {
            for (int ox = 0; ox < W_out; ox++) {
                float acc = 0.0f;
                for (int ci = 0; ci < C_in; ci++) {
                    const float* in_ch  = in + ci * H_in * W_in;
                    const float* wslice = w + (co * C_in + ci) * K * K;
                    for (int ky = 0; ky < K; ky++) {
                        for (int kx = 0; kx < K; kx++) {
                            int iy = oy * stride + ky - pad;
                            int ix = ox * stride + kx - pad;
                            if ((unsigned)iy < (unsigned)H_in &&
                                (unsigned)ix < (unsigned)W_in) {
                                acc += wslice[ky * K + kx]
                                       * in_ch[iy * W_in + ix];
                            }
                        }
                    }
                }
                out[co * H_out * W_out + oy * W_out + ox] = acc;
            }
        }
    }
}

/* ================================================================
 * conv2d_neon_4ch — NEON-accelerated 3×3 conv2d
 *
 * WEIGHT LAYOUT (repacked by export_model.py):
 *   [C_out/4][C_in*K*K][4]  (flat float32)
 *
 *   For 4-output-channel group g and kernel position p = ci*K*K + ky*K + kx,
 *   the 4 weights (for channels g*4, g*4+1, g*4+2, g*4+3) are at
 *   indices (g * C_in*K*K + p) * 4 ... +3  — CONTIGUOUS → vld1q_f32.
 *
 * STRATEGY:
 *   Outer: iterate output positions (oy, ox) then channel groups g.
 *   Inner: for each (ci, ky, kx) load input scalar + 4 weights,
 *          accumulate with vmlaq_n_f32.
 *   Store: 4 scalars to non-contiguous output planes (scatter).
 *
 * BOUNDARY:
 *   Boundary output pixels (touching zero-padding) handled with
 *   unsigned comparison trick. Interior pixels in a split fast-path.
 *
 * NOTE: C_out must be divisible by 4. Our network: 16, 32, 64 ✓
 * ================================================================ */
void conv2d_neon_4ch(const float* in, int H_in, int W_in, int C_in,
                     const float* w_rep, int C_out, int K,
                     int stride, int pad, float* out) {
    int H_out = (H_in + 2 * pad - K) / stride + 1;
    int W_out = (W_in + 2 * pad - K) / stride + 1;
    int HW_out = H_out * W_out;
    int HW_in  = H_in  * W_in;
    int CinKK  = C_in * K * K;           /* weights per output channel */
    int n_grp  = C_out / 4;             /* number of 4-ch groups */

    /* Zero-initialise output buffer */
    for (int i = 0; i < C_out * HW_out; i++) out[i] = 0.0f;

    for (int g = 0; g < n_grp; g++) {
        int co = g * 4;
        /* Pointer to this group's weights: [CinKK][4] */
        const float* wg = w_rep + g * CinKK * 4;

        /* Output channel base pointers */
        float* o0 = out + (co + 0) * HW_out;
        float* o1 = out + (co + 1) * HW_out;
        float* o2 = out + (co + 2) * HW_out;
        float* o3 = out + (co + 3) * HW_out;

        for (int oy = 0; oy < H_out; oy++) {
            for (int ox = 0; ox < W_out; ox++) {
                float32x4_t acc = vdupq_n_f32(0.0f);

                for (int ci = 0; ci < C_in; ci++) {
                    const float* in_ch  = in + ci * HW_in;
                    const float* wci    = wg + ci * K * K * 4;

                    for (int ky = 0; ky < K; ky++) {
                        int iy = oy * stride + ky - pad;
                        for (int kx = 0; kx < K; kx++) {
                            int ix = ox * stride + kx - pad;
                            /* Zero-padding: skip out-of-bounds */
                            if ((unsigned)iy < (unsigned)H_in &&
                                (unsigned)ix < (unsigned)W_in) {
                                float   in_val = in_ch[iy * W_in + ix];
                                /* 4 weights for 4 output channels, contiguous */
                                float32x4_t wv = vld1q_f32(wci + (ky * K + kx) * 4);
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

/* ================================================================
 * conv1x1 — 1×1 conv with bias (YOLO head, NEON vectorised)
 *
 * in  : [C_in][H][W]
 * w   : [C_out][C_in]   (standard layout, C_out=6 here)
 * b   : [C_out]
 * out : [C_out][H][W]
 * ================================================================ */
void conv1x1(const float* in, int H, int W, int C_in,
             const float* w, const float* b, int C_out,
             float* out) {
    int HW = H * W;
    for (int co = 0; co < C_out; co++) {
        const float* wrow = w + co * C_in;
        float*       och  = out + co * HW;
        float        bias = b[co];
        /* Dot product of wrow[C_in] against each column of in[C_in][HW] */
        for (int pos = 0; pos < HW; pos++) {
            float acc = bias;
            int ci = 0;
            float32x4_t vacc = vdupq_n_f32(0.0f);
            for (; ci <= C_in - 4; ci += 4) {
                float32x4_t vw = vld1q_f32(wrow + ci);
                /* in[ci..ci+3][pos] — non-contiguous, use gather */
                float32x4_t vi = {in[(ci+0)*HW + pos],
                                   in[(ci+1)*HW + pos],
                                   in[(ci+2)*HW + pos],
                                   in[(ci+3)*HW + pos]};
                vacc = vmlaq_f32(vacc, vw, vi);
            }
            /* Horizontal add of NEON accumulator */
            float32x2_t s = vadd_f32(vget_low_f32(vacc), vget_high_f32(vacc));
            acc += vget_lane_f32(vpadd_f32(s, s), 0);
            /* Scalar tail */
            for (; ci < C_in; ci++) {
                acc += wrow[ci] * in[ci * HW + pos];
            }
            och[pos] = acc;
        }
    }
}

/* ================================================================
 * yolo_decode
 * head_out[6][grid_H][grid_W] in CHW format.
 * ================================================================ */

static float box_iou(const BBox& a, const BBox& b) {
    float ax1 = a.x - a.w * 0.5f, ay1 = a.y - a.h * 0.5f;
    float ax2 = a.x + a.w * 0.5f, ay2 = a.y + a.h * 0.5f;
    float bx1 = b.x - b.w * 0.5f, by1 = b.y - b.h * 0.5f;
    float bx2 = b.x + b.w * 0.5f, by2 = b.y + b.h * 0.5f;
    float iw = fminf2(ax2, bx2) - fmaxf2(ax1, bx1);
    float ih = fminf2(ay2, by2) - fmaxf2(ay1, by1);
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    float inter = iw * ih;
    return inter / (a.w * a.h + b.w * b.h - inter);
}

int yolo_decode(const float* head_out, int grid_H, int grid_W,
                float obj_thresh, BBox* boxes, int max_boxes) {
    int GW = grid_W, GHW = grid_H * grid_W;
    BBox tmp[64]; int ntmp = 0;

    for (int gy = 0; gy < grid_H && ntmp < 64; gy++) {
        for (int gx = 0; gx < grid_W && ntmp < 64; gx++) {
            int pos = gy * GW + gx;
            float obj = mini_sigmoidf(head_out[4 * GHW + pos]);
            float cls = mini_sigmoidf(head_out[5 * GHW + pos]);
            float conf = obj * cls;
            if (conf < obj_thresh) continue;
            float cx = ((float)gx + mini_sigmoidf(head_out[0 * GHW + pos])) / grid_W;
            float cy = ((float)gy + mini_sigmoidf(head_out[1 * GHW + pos])) / grid_H;
            float bw = mini_expf(head_out[2 * GHW + pos]) / grid_W;
            float bh = mini_expf(head_out[3 * GHW + pos]) / grid_H;
            tmp[ntmp++] = {cx, cy, bw, bh, conf, cls};
        }
    }

    /* Insertion sort by conf descending */
    for (int i = 1; i < ntmp; i++) {
        BBox key = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j].conf < key.conf) { tmp[j+1] = tmp[j]; j--; }
        tmp[j+1] = key;
    }

    /* NMS */
    int nout = 0;
    bool suppressed[64] = {};
    for (int i = 0; i < ntmp && nout < max_boxes; i++) {
        if (suppressed[i]) continue;
        boxes[nout++] = tmp[i];
        for (int j = i + 1; j < ntmp; j++) {
            if (!suppressed[j] && box_iou(tmp[i], tmp[j]) > 0.45f)
                suppressed[j] = true;
        }
    }
    return nout;
}
