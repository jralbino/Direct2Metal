/* src/yolo_v8n.cpp — YOLOv8n COCO model graph + inference loop body.
 *
 * Split from kernel.cpp on 2026-05-20 (Step 4 of the BSP/app boundary,
 * see GOALS.md G1). Will move to app/yolo_v8n_coco/ in Step 5.
 *
 * V170 — YOLOv8n @ 256². v8 head is heavier than v5 (3 convs × 3 levels
 * for box+cls), so dropping from 320 to 256 buys ~1.5× compute without
 * much mAP loss. Stride 32 → S32=8, all spatial dims stay integer.
 * YOLO_IN comes from bsp.h (Step 3.5: single source of truth shared with
 * the debayer). */
#include <stdint.h>
#include <cstddef>
#include <arm_neon.h>
#include "ops.h"
#include "multicore.h"
#include "safety_config.h"
#include "watchdog.h"
#include "camera.h"
#include "hud.h"
#include "bsp.h"
#include "yolo_v8n.h"
#include "weights_crc.h"           /* WEIGHTS_CRC32 + SIZE        (FP32 blob) */
#include "weights_int8_crc.h"      /* WEIGHTS_INT8_CRC32 + SIZE   (Tier 1 W8A32 blob) */
#include "weights_int8_w8a8_crc.h" /* WEIGHTS_INT8_W8A8_CRC32 + SIZE (Tier 2 W8A8 blob) */

/* Weight blobs + test image — defined by app/yolo_v8n_coco/data.s via .incbin. */
extern "C" const float  weights_start[];
extern "C" const float  weights_end[];
extern "C" const int8_t weights_int8_start[];        /* Tier 1 — unused outside USE_INT8_WEIGHTS */
extern "C" const int8_t weights_int8_end[];
extern "C" const int8_t weights_int8_w8a8_start[];   /* Tier 2 — unused outside USE_INT8_W8A8  */
extern "C" const int8_t weights_int8_w8a8_end[];
extern "C" const float  test_image[];

#define YOLO_S2  (YOLO_IN / 2)
#define YOLO_S4  (YOLO_IN / 4)
#define YOLO_S8  (YOLO_IN / 8)
#define YOLO_S16 (YOLO_IN / 16)
#define YOLO_S32 (YOLO_IN / 32)

#define DFL_REG_MAX 16   /* v8 default; box ch = 4 * REG_MAX */

static float buf_A[2000000]; static float buf_B[2000000]; static float scratch[2000000];
static float cam_frame[3 * YOLO_IN * YOLO_IN];

/* Skip connections + per-level head inputs (v8 PAN). */
static float save_L4   [64  * YOLO_S8  * YOLO_S8];    /* L4 (P3 backbone)     */
static float save_L6   [128 * YOLO_S16 * YOLO_S16];   /* L6 (P4 backbone)     */
static float save_SPPF [256 * YOLO_S32 * YOLO_S32];   /* L9 SPPF out          */
static float save_P4mid[128 * YOLO_S16 * YOLO_S16];   /* L12 (mid neck)       */
static float save_P3   [64  * YOLO_S8  * YOLO_S8];    /* L15 → P3 head input  */
static float save_P4   [128 * YOLO_S16 * YOLO_S16];   /* L18 → P4 head input  */

/* Head per-level scratch. Sized for P3 (largest spatial grid). */
static float head_box [64 * YOLO_S8 * YOLO_S8];
static float head_cls [80 * YOLO_S8 * YOLO_S8];
static float head_tmp [80 * YOLO_S8 * YOLO_S8];

static float mini_exp(float x) {
    if (x > 88.0f) { return 3.40282347e+38f; }
    if (x < -88.0f) { return 0.0f; }
    float z = x * 1.44269504f; int32_t k = (z >= 0.0f) ? (int32_t)(z + 0.5f) : (int32_t)(z - 0.5f);
    float r = x - (float)k * 0.69314718f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.04166667f + r * 0.00833333f))));
    union { float f; uint32_t u; } bits; bits.u = (uint32_t)(k + 127) << 23; return bits.f * p;
}
static float fast_sigmoid(float x) { return 1.0f / (1.0f + mini_exp(-x)); }

static inline float32x4_t k_neon_expf4(float32x4_t x) {
    x = vminq_f32(x, vdupq_n_f32(88.0f)); x = vmaxq_f32(x, vdupq_n_f32(-88.0f));
    float32x4_t z = vmulq_n_f32(x, 1.44269504f); int32x4_t k = vcvtaq_s32_f32(z);
    float32x4_t r = vmlsq_n_f32(x, vcvtq_f32_s32(k), 0.69314718f);
    float32x4_t p = vdupq_n_f32(0.00833333f);
    p = vmlaq_f32(vdupq_n_f32(0.04166667f), r, p); p = vmlaq_f32(vdupq_n_f32(0.16666667f), r, p);
    p = vmlaq_f32(vdupq_n_f32(0.5f), r, p); p = vmlaq_f32(vdupq_n_f32(1.0f), r, p); p = vmlaq_f32(vdupq_n_f32(1.0f), r, p);
    int32x4_t pow2 = vshlq_n_s32(vaddq_s32(k, vdupq_n_s32(127)), 23);
    return vmulq_f32(vreinterpretq_f32_s32(pow2), p);
}
static inline float32x4_t k_neon_sigmoidf4(float32x4_t x) {
    float32x4_t neg_x = vnegq_f32(x);
    float32x4_t ex = k_neon_expf4(neg_x);
    float32x4_t denom = vaddq_f32(ex, vdupq_n_f32(1.0f));
    float32x4_t recip = vrecpeq_f32(denom);
    recip = vmulq_f32(recip, vrecpsq_f32(denom, recip));
    recip = vmulq_f32(recip, vrecpsq_f32(denom, recip));
    return recip;
}
static void copy_tensor(const float* src, float* dst, int n) {
    int i = 0; for (; i <= n - 4; i += 4) vst1q_f32(dst + i, vld1q_f32(src + i));
    for (; i < n; i++) dst[i] = src[i];
}
static void concat_tensor(const float* src1, int c1, const float* src2, int c2, float* dst, int hw) {
    copy_tensor(src1, dst, c1 * hw); copy_tensor(src2, dst + c1 * hw, c2 * hw);
}
struct WeightStream {
    const uint8_t* ptr; WeightStream(const float* start) : ptr((const uint8_t*)start) {}
    const float* next(int expected_count, const char* layer_name) {
        uint32_t actual_count = *((const uint32_t*)ptr); ptr += 4;
        if (actual_count != (uint32_t)expected_count) { uart_puts("\n[FATAL ERROR] "); uart_puts(layer_name); while(1); }
        const float* p = (const float*)ptr; ptr += actual_count * 4; return p;
    }
};

/* ──────────────────────────────────────────────────────────────────────────
 * G2 — Tier 1 (W8A32) scaffolding.
 *
 * Parallel reader for weights_int8.bin (per export_int8.py format):
 *
 *   per Conv2d:
 *     uint32  n_w     (count of int8 elements, NEON-repacked)
 *     float   scale   (per-tensor symmetric: w_fp = w_int8 * scale)
 *     int8    weights[n_w]            ── unaligned tail allowed on AArch64
 *     uint32  n_b     (= C_out)
 *     float   bias[n_b]
 *
 * Activations + accumulators stay FP32 — only the weight load is int8.
 * The dequant helper below converts a packed block of int8 weights into
 * the FP32 layout the existing conv2d_partial_8ch / partial / 1x1 kernels
 * expect. Tier 1 gain comes from reduced L1 cache pressure on large
 * layers; the real ×3-4 fps lift waits for Tier 2 (W8A8).
 * ────────────────────────────────────────────────────────────────────── */
struct WeightStreamINT8 {
    struct Layer {
        int           n_w;
        float         scale;
        const int8_t* weights;
        int           n_b;
        const float*  bias;
    };
    const uint8_t* ptr;
    WeightStreamINT8(const int8_t* start) : ptr((const uint8_t*)start) {}

    Layer next(int expected_w, int expected_b, const char* layer_name) {
        Layer L;
        L.n_w = (int)(*((const uint32_t*)ptr)); ptr += 4;
        if (L.n_w != expected_w) {
            uart_puts("\n[FATAL INT8 W] "); uart_puts(layer_name); while (1);
        }
        L.scale   = *((const float*)ptr);   ptr += 4;
        L.weights = (const int8_t*)ptr;     ptr += L.n_w;            /* tail may be unaligned */
        L.n_b     = (int)(*((const uint32_t*)ptr)); ptr += 4;
        if (L.n_b != expected_b) {
            uart_puts("\n[FATAL INT8 B] "); uart_puts(layer_name); while (1);
        }
        L.bias    = (const float*)ptr;      ptr += L.n_b * 4;
        return L;
    }
};

/* Dequant: int8[n] * scale → float32[n], 16-lane NEON. n need not be a
 * multiple of 16 — tail handled scalar. dst must be 16-byte aligned for
 * the vector stores; the weight buffers are statically aligned via .align
 * in data.s and the per-layer NEON-repacked layout is already aligned. */
static void dequant_int8_to_fp32_neon(const int8_t* w_int8, int n, float scale, float* dst) {
    float32x4_t vscale = vdupq_n_f32(scale);
    int i = 0;
    for (; i <= n - 16; i += 16) {
        int8x16_t  v8   = vld1q_s8(w_int8 + i);
        int16x8_t  lo16 = vmovl_s8(vget_low_s8(v8));
        int16x8_t  hi16 = vmovl_s8(vget_high_s8(v8));
        int32x4_t  a32  = vmovl_s16(vget_low_s16(lo16));
        int32x4_t  b32  = vmovl_s16(vget_high_s16(lo16));
        int32x4_t  c32  = vmovl_s16(vget_low_s16(hi16));
        int32x4_t  d32  = vmovl_s16(vget_high_s16(hi16));
        vst1q_f32(dst + i +  0, vmulq_f32(vcvtq_f32_s32(a32), vscale));
        vst1q_f32(dst + i +  4, vmulq_f32(vcvtq_f32_s32(b32), vscale));
        vst1q_f32(dst + i +  8, vmulq_f32(vcvtq_f32_s32(c32), vscale));
        vst1q_f32(dst + i + 12, vmulq_f32(vcvtq_f32_s32(d32), vscale));
    }
    for (; i < n; i++) dst[i] = (float)w_int8[i] * scale;
}

/* ──────────────────────────────────────────────────────────────────────────
 * G2 Tier 2 (W8A8) — reader for weights_int8_w8a8.bin.
 *
 * Per Conv2d layout (from tools/calibrate_int8.py):
 *   uint32  n_w
 *   float   scale_w
 *   float   scale_in
 *   float   scale_out
 *   int8    weights[n_w]
 *   uint32  n_b
 *   int32   bias[n_b]    (pre-multiplied by 1/(scale_in*scale_w))
 * ────────────────────────────────────────────────────────────────────── */
struct WeightStreamW8A8 {
    struct Layer {
        int            n_w;
        float          scale_w;
        float          scale_in;
        float          scale_out;
        const int8_t*  weights;
        int            n_b;
        const int32_t* bias;
    };
    const uint8_t* ptr;
    WeightStreamW8A8(const int8_t* start) : ptr((const uint8_t*)start) {}

    Layer next(int expected_w, int expected_b, const char* layer_name) {
        Layer L;
        L.n_w = (int)(*((const uint32_t*)ptr)); ptr += 4;
        if (L.n_w != expected_w) {
            uart_puts("\n[FATAL W8A8 W] "); uart_puts(layer_name); while (1);
        }
        L.scale_w   = *((const float*)ptr); ptr += 4;
        L.scale_in  = *((const float*)ptr); ptr += 4;
        L.scale_out = *((const float*)ptr); ptr += 4;
        L.weights   = (const int8_t*)ptr;   ptr += L.n_w;
        L.n_b       = (int)(*((const uint32_t*)ptr)); ptr += 4;
        if (L.n_b != expected_b) {
            uart_puts("\n[FATAL W8A8 B] "); uart_puts(layer_name); while (1);
        }
        L.bias      = (const int32_t*)ptr;  ptr += L.n_b * 4;
        return L;
    }
};

/* ──────────────────────────────────────────────────────────────────────────
 * G2 Tier 2 — int8 buffer helpers (concat / copy / upsample / maxpool /
 * residual) and the K-aware conv dispatch. All are `[[maybe_unused]]`
 * because they only fire under USE_INT8_W8A8; the FP32 / W8A32 builds
 * include them as compiled-but-unused symbols (caught early on syntax).
 * ────────────────────────────────────────────────────────────────────── */

[[maybe_unused]]
static void copy_tensor_i8(const int8_t* src, int8_t* dst, int n) {
    int i = 0;
    for (; i <= n - 16; i += 16) vst1q_s8(dst + i, vld1q_s8(src + i));
    for (; i < n; i++) dst[i] = src[i];
}

[[maybe_unused]]
static void concat_tensor_i8(const int8_t* src1, int c1,
                             const int8_t* src2, int c2,
                             int8_t* dst, int hw) {
    copy_tensor_i8(src1, dst,           c1 * hw);
    copy_tensor_i8(src2, dst + c1 * hw, c2 * hw);
}

[[maybe_unused]]
static void upsample2x_nearest_i8(const int8_t* in, int8_t* out, int H, int W, int C) {
    int W2 = W * 2;
    for (int c = 0; c < C; c++) {
        const int8_t* in_ch  = in  + c * H * W;
        int8_t*       out_ch = out + c * (H * 2) * W2;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int8_t v = in_ch[y * W + x];
                out_ch[(y*2  ) * W2 + (x*2  )] = v;
                out_ch[(y*2  ) * W2 + (x*2+1)] = v;
                out_ch[(y*2+1) * W2 + (x*2  )] = v;
                out_ch[(y*2+1) * W2 + (x*2+1)] = v;
            }
        }
    }
}

[[maybe_unused]]
static void maxpool5x5_s1_p2_i8(const int8_t* in, int8_t* out, int H, int W, int C) {
    /* 5×5 stride-1 padding-2 → output size == input size. Out-of-bounds
     * (negative or ≥ H/W) treated as -128 (int8 minimum, neutral for max). */
    for (int c = 0; c < C; c++) {
        const int8_t* in_ch  = in  + c * H * W;
        int8_t*       out_ch = out + c * H * W;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int8_t m = -128;
                for (int ky = -2; ky <= 2; ky++) {
                    int iy = y + ky;
                    if ((unsigned)iy >= (unsigned)H) continue;
                    for (int kx = -2; kx <= 2; kx++) {
                        int ix = x + kx;
                        if ((unsigned)ix >= (unsigned)W) continue;
                        int8_t v = in_ch[iy * W + ix];
                        if (v > m) m = v;
                    }
                }
                out_ch[y * W + x] = m;
            }
        }
    }
}

/* Residual add: x_out[i] = saturate( x_out[i] + x_in[i] * (x_in_scale /
 * x_out_scale) ), in-place on x_out. fp32 path because the two operands
 * may carry different per-tensor scales — saturated int8 add would lose
 * the relative magnitudes. */
[[maybe_unused]]
static void w8a8_residual_add(int8_t* x_out, const int8_t* x_in, int n,
                              float x_out_scale, float x_in_scale) {
    float ratio = x_in_scale / x_out_scale;
    for (int i = 0; i < n; i++) {
        float s = (float)x_out[i] + (float)x_in[i] * ratio;
        int32_t q = (int32_t)(s + (s >= 0 ? 0.5f : -0.5f));
        if (q >  127) q =  127;
        if (q < -128) q = -128;
        x_out[i] = (int8_t)q;
    }
}

/* K-aware conv dispatch — picks the int8 kernel that matches the weight
 * repack layout chosen by tools/calibrate_int8.py::emit_conv_w8a8. */
[[maybe_unused]]
static inline void w8a8_conv2d_dispatch(const int8_t* in, int H, int W, int C_in,
                                        const WeightStreamW8A8::Layer& L,
                                        int C_out, int K, int stride, int pad,
                                        bool do_silu, int8_t* out) {
    if (K > 1 && C_out >= 32 && C_out % 8 == 0) {
        conv2d_neon_8ch_int8(in, H, W, C_in, L.weights, C_out, K, stride, pad,
                             L.scale_w, L.scale_in, L.scale_out, L.bias, do_silu, out);
    } else if (K > 1 && C_out % 4 == 0) {
        conv2d_neon_4ch_int8(in, H, W, C_in, L.weights, C_out, K, stride, pad,
                             L.scale_w, L.scale_in, L.scale_out, L.bias, do_silu, out);
    } else {
        /* K == 1 (only K==1 in YOLOv8n falls into this branch). */
        conv1x1_int8(in, H, W, C_in, L.weights, L.bias,
                     L.scale_w, L.scale_in, L.scale_out, C_out, do_silu, out);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * G2 Tier 1 — unified layer handle + macros that switch the stream type
 * and conv functions at compile time on -DUSE_INT8_WEIGHTS. Both paths
 * share the same model graph in run_yolo_complete / c2f / sppf.
 * ────────────────────────────────────────────────────────────────────── */
#ifdef USE_INT8_WEIGHTS
/* INT8 path. Dequant scratch is a single shared buffer sized for the
 * largest layer in YOLOv8n@256 (L7 = 256*128*9 = 294 912 floats). 350 K
 * floats = 1.4 MB, comfortable margin. The kernel does not need it to
 * persist between conv calls — each CONV* macro overwrites it. */
static float dequant_scratch[350000];

typedef WeightStreamINT8::Layer LayerHandle;

static inline void parallel_conv2d_int8(const float* in, int H, int W, int C_in,
                                        const LayerHandle& L,
                                        int C_out, int K, int stride, int pad,
                                        bool do_silu, float* out) {
    dequant_int8_to_fp32_neon(L.weights, L.n_w, L.scale, dequant_scratch);
    parallel_conv2d(in, H, W, C_in, dequant_scratch, L.bias, C_out, K, stride, pad, do_silu, out);
}

static inline void parallel_conv1x1_int8(const float* in, int H, int W, int C_in,
                                         const LayerHandle& L,
                                         int C_out, bool do_silu, float* out) {
    dequant_int8_to_fp32_neon(L.weights, L.n_w, L.scale, dequant_scratch);
    parallel_conv1x1(in, H, W, C_in, dequant_scratch, L.bias, C_out, do_silu, out);
}

#define WS_TYPE       WeightStreamINT8
#define WS_INIT(name) WeightStreamINT8 name(weights_int8_start)
#define LAYER_LOAD(ws, nw, nb, tag) (ws).next((nw), (nb), (tag))
#define CONV2D(in, H, W, ci, L, co, K, s, p, silu, out) \
    parallel_conv2d_int8((in), (H), (W), (ci), (L), (co), (K), (s), (p), (silu), (out))
#define CONV1X1(in, H, W, ci, L, co, silu, out) \
    parallel_conv1x1_int8((in), (H), (W), (ci), (L), (co), (silu), (out))

#else
/* FP32 path. LayerHandle bundles the two ws.next() results so c2f and
 * sppf can hold them across the bottleneck loop without exposing the
 * stream API to the macros. */
struct LayerHandle { const float* w; const float* b; };

static inline LayerHandle fp32_layer_load(WeightStream& ws, int nw, int nb, const char* tag) {
    LayerHandle L;
    L.w = ws.next(nw, tag);
    L.b = ws.next(nb, tag);
    return L;
}

#define WS_TYPE       WeightStream
#define WS_INIT(name) WeightStream name(weights_start)
#define LAYER_LOAD(ws, nw, nb, tag) fp32_layer_load((ws), (nw), (nb), (tag))
#define CONV2D(in, H, W, ci, L, co, K, s, p, silu, out) \
    parallel_conv2d((in), (H), (W), (ci), (L).w, (L).b, (co), (K), (s), (p), (silu), (out))
#define CONV1X1(in, H, W, ci, L, co, silu, out) \
    parallel_conv1x1((in), (H), (W), (ci), (L).w, (L).b, (co), (silu), (out))
#endif

struct Box { float x, y, w, h, conf; int cls; };
static Box preds[MAX_PREDS]; static int num_preds = 0;

static float calculate_iou(const Box& a, const Box& b) {
    float x1_int = (a.x - a.w/2) > (b.x - b.w/2) ? (a.x - a.w/2) : (b.x - b.w/2);
    float y1_int = (a.y - a.h/2) > (b.y - b.h/2) ? (a.y - a.h/2) : (b.y - b.h/2);
    float x2_int = (a.x + a.w/2) < (b.x + b.w/2) ? (a.x + a.w/2) : (b.x + b.w/2);
    float y2_int = (a.y + a.h/2) < (b.y + b.h/2) ? (a.y + a.h/2) : (b.y + b.h/2);
    float w_int = x2_int - x1_int; float h_int = y2_int - y1_int;
    if (w_int <= 0 || h_int <= 0) return 0.0f;
    float area_int = w_int * h_int; return area_int / (a.w * a.h + b.w * b.h - area_int);
}

/* ------------------------------------------------------------------ *
 * YOLOv8 anchor-free DFL decoder.
 *
 * Reads the per-level box (64ch, CHW) and class (80ch, CHW, raw logits)
 * grids produced by Detect.cv2[i] and Detect.cv3[i]. For every cell we:
 *   1. find argmax over 80 raw cls logits, sigmoid it, prefilter.
 *   2. for each of 4 sides (l, t, r, b), softmax over 16 bins and take
 *      the integral E[k] = Σ k * softmax(box[s*16:(s+1)*16]).
 *   3. recover (x, y, w, h) in YOLO_IN-space using anchor at cell centre.
 * Pushes preds onto the shared preds[] buffer (consumed by NMS below).
 * ------------------------------------------------------------------ */
static void decode_v8_dfl(const float* box_chw, const float* cls_chw,
                          int gh, int gw, int stride) {
    int grd = gh * gw;
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            int idx = cy * gw + cx;

            float max_logit = -1e30f; int best_cls = -1;
            for (int c = 0; c < NUM_CLASSES; c++) {
                float v = cls_chw[c * grd + idx];
                if (v > max_logit) { max_logit = v; best_cls = c; }
            }
            float p = fast_sigmoid(max_logit);
            if (p <= OBJ_PRE_THRESH) continue;
            if (num_preds >= MAX_PREDS) return;

            float dist[4];
            for (int s = 0; s < 4; s++) {
                float lmax = -1e30f;
                for (int k = 0; k < DFL_REG_MAX; k++) {
                    float lv = box_chw[(s * DFL_REG_MAX + k) * grd + idx];
                    if (lv > lmax) lmax = lv;
                }
                float exps[DFL_REG_MAX]; float esum = 0.0f;
                for (int k = 0; k < DFL_REG_MAX; k++) {
                    float e = mini_exp(box_chw[(s * DFL_REG_MAX + k) * grd + idx] - lmax);
                    exps[k] = e; esum += e;
                }
                float inv = 1.0f / esum; float integral = 0.0f;
                for (int k = 0; k < DFL_REG_MAX; k++) integral += exps[k] * (float)k * inv;
                dist[s] = integral;
            }
            float l = dist[0], t = dist[1], r = dist[2], b = dist[3];
            float ax = (float)cx + 0.5f, ay = (float)cy + 0.5f;
            float x_min = ax - l, y_min = ay - t;
            float x_max = ax + r, y_max = ay + b;
            preds[num_preds].x = (x_min + x_max) * 0.5f * (float)stride;
            preds[num_preds].y = (y_min + y_max) * 0.5f * (float)stride;
            preds[num_preds].w = (x_max - x_min) * (float)stride;
            preds[num_preds].h = (y_max - y_min) * (float)stride;
            preds[num_preds].conf = p;
            preds[num_preds].cls  = best_cls;
            num_preds++;
        }
    }
}

/* ------------------------------------------------------------------ *
 * YOLOv8 C2f block.
 *
 * forward(x):
 *     y = cv1(x)                         # 1x1, c_in → 2*c_hidden
 *     a, b = split(y, c_hidden, dim=1)
 *     outs = [a, b]
 *     for m in bottlenecks:
 *         b = m(b)                       # b may be added to its input
 *                                        # via residual when shortcut=True
 *         outs.append(b)
 *     return cv2(cat(outs, dim=1))       # 1x1, (2+n)*c_hidden → c_out
 *
 * Layout in temp[]:
 *   [0 .. c_hidden*hw)            slot 0 = a
 *   [c_hidden*hw .. 2*c_hidden*hw) slot 1 = b
 *   [2*c_hidden*hw .. 3*c_hidden*hw) slot 2 = m0(b)
 *   ...
 *   total length = (2 + n_depth) * c_hidden * hw floats
 *
 * The two 3x3 Bottleneck convs alternate `out` and the slot buffer; `in`,
 * `out`, `temp` must be three distinct buffers.
 * ------------------------------------------------------------------ */
void c2f_real_inference(float* in, float* out, float* temp,
                        int h, int w, int c_in, int c_out, int n_depth,
                        bool shortcut, WS_TYPE& ws, const char* prefix) {
    (void)prefix;
    int c_hidden = c_out / 2;
    int hw = h * w;
    int hw_h = hw * c_hidden;
    int c_concat = (2 + n_depth) * c_hidden;

    LayerHandle Lcv1 = LAYER_LOAD(ws, c_in * (2 * c_hidden), 2 * c_hidden, "C2f_CV1");
    LayerHandle Lcv2 = LAYER_LOAD(ws, c_concat * c_out,      c_out,        "C2f_CV2");

    /* cv1: writes (a | b) contiguously into temp slots 0 and 1. */
    CONV1X1(in, h, w, c_in, Lcv1, 2 * c_hidden, true, temp);

    for (int i = 0; i < n_depth; i++) {
        LayerHandle Lb1 = LAYER_LOAD(ws, c_hidden * c_hidden * 9, c_hidden, "Bot_CV1");
        LayerHandle Lb2 = LAYER_LOAD(ws, c_hidden * c_hidden * 9, c_hidden, "Bot_CV2");

        float* x_in  = temp + (1 + i) * hw_h;          /* previous slot */
        float* x_mid = out;                            /* scratch */
        float* x_out = temp + (2 + i) * hw_h;          /* new slot */

        CONV2D(x_in,  h, w, c_hidden, Lb1, c_hidden, 3, 1, 1, true, x_mid);
        CONV2D(x_mid, h, w, c_hidden, Lb2, c_hidden, 3, 1, 1, true, x_out);

        if (shortcut) {
            int k = 0;
            for (; k <= hw_h - 4; k += 4) {
                float32x4_t a = vld1q_f32(x_in  + k);
                float32x4_t v = vld1q_f32(x_out + k);
                vst1q_f32(x_out + k, vaddq_f32(a, v));
            }
            for (; k < hw_h; k++) x_out[k] += x_in[k];
        }
    }

    CONV1X1(temp, h, w, c_concat, Lcv2, c_out, true, out);
}

void sppf_real_inference(float* in, float* out, float* temp, int h, int w, int c, WS_TYPE& ws) {
    int c_hidden = c / 2; int hw = h * w; int hw_hidden = c_hidden * hw;
    LayerHandle L1 = LAYER_LOAD(ws, c * c_hidden,            c_hidden, "SPPF_1");
    LayerHandle L2 = LAYER_LOAD(ws, (c_hidden * 4) * c,      c,        "SPPF_2");

    float* cv1_out = temp; CONV1X1(in, h, w, c, L1, c_hidden, true, cv1_out);
    float* m1 = temp + hw_hidden; float* m2 = temp + 2*hw_hidden; float* m3 = temp + 3*hw_hidden;

    maxpool5x5_s1_p2(cv1_out, m1, h, w, c_hidden); maxpool5x5_s1_p2(m1, m2, h, w, c_hidden); maxpool5x5_s1_p2(m2, m3, h, w, c_hidden);
    CONV1X1(temp, h, w, c_hidden * 4, L2, c, true, out);
}

static uint32_t crc32_lut[256];
static bool crc32_lut_ready = false;

static void crc32_init() {
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc32_lut[i] = c;
    }
    crc32_lut_ready = true;
}

static uint32_t crc32_sw(const uint8_t* data, size_t len) {
    if (!crc32_lut_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = crc32_lut[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

static int heartbeat_counter = 0;

static void draw_tensor_image_fullscreen(const float* img) {
    const float* dst_r = img;
    const float* dst_g = img + (YOLO_IN * YOLO_IN);
    const float* dst_b = img + (2 * YOLO_IN * YOLO_IN);
    for (int y = 0; y < 480; y++) {
        int src_y = (y * YOLO_IN) / 480;
        for (int x = 0; x < 640; x++) {
            int src_x = (x * YOLO_IN) / 640;
            int idx = src_y * YOLO_IN + src_x;
            int r = (int)(dst_r[idx] * 255.0f); int g = (int)(dst_g[idx] * 255.0f); int b = (int)(dst_b[idx] * 255.0f);

            if (r < 0) { r = 0; }
            if (r > 255) { r = 255; }
            if (g < 0) { g = 0; }
            if (g > 255) { g = 255; }
            if (b < 0) { b = 0; }
            if (b > 255) { b = 255; }

            uint32_t color = 0xFF000000 | (b << 16) | (g << 8) | r; draw_pixel(x, y, color);
        }
    }
}

static int global_frame_counter = 1;

void run_yolo_complete() {
    watchdog_kick();
    unsigned long f = get_timer_freq(); unsigned long t_start = get_timer_count();
    uart_puts("[F"); uart_dec(global_frame_counter); uart_puts("] ");
    global_frame_counter++;

    WS_INIT(ws); num_preds = 0;

    static bool weights_verified = false;
    if (!weights_verified && WEIGHTS_CRC32 != 0x00000000U) {
        size_t wsz = (size_t)((const uint8_t*)weights_end - (const uint8_t*)weights_start);
        uint32_t actual = crc32_sw((const uint8_t*)weights_start, wsz);
        SAFETY_ASSERT(actual == WEIGHTS_CRC32, "weights CRC mismatch");
        weights_verified = true;
    }

    /* G2 Tier 1 — verify INT8 (W8A32) blob shipped intact. */
    static bool weights_int8_verified = false;
    if (!weights_int8_verified && WEIGHTS_INT8_CRC32 != 0x00000000U) {
        size_t sz = (size_t)((const uint8_t*)weights_int8_end - (const uint8_t*)weights_int8_start);
        SAFETY_ASSERT(sz == WEIGHTS_INT8_SIZE, "weights_int8 size mismatch");
        uint32_t actual = crc32_sw((const uint8_t*)weights_int8_start, sz);
        SAFETY_ASSERT(actual == WEIGHTS_INT8_CRC32, "weights_int8 CRC mismatch");
        uart_puts("[INT8] blob OK\n");
        weights_int8_verified = true;
    }

    /* G2 Tier 2 — verify W8A8 blob shipped intact. */
    static bool weights_w8a8_verified = false;
    if (!weights_w8a8_verified && WEIGHTS_INT8_W8A8_CRC32 != 0x00000000U) {
        size_t sz = (size_t)((const uint8_t*)weights_int8_w8a8_end -
                              (const uint8_t*)weights_int8_w8a8_start);
        SAFETY_ASSERT(sz == WEIGHTS_INT8_W8A8_SIZE, "weights_int8_w8a8 size mismatch");
        uint32_t actual = crc32_sw((const uint8_t*)weights_int8_w8a8_start, sz);
        SAFETY_ASSERT(actual == WEIGHTS_INT8_W8A8_CRC32, "weights_int8_w8a8 CRC mismatch");
        uart_puts("[W8A8] blob OK\n");
        weights_w8a8_verified = true;
    }

    if (g_use_camera) {
        if (bsp_frame_acquire()) {
            debayer_raw10_to_chw_yolo(cam_frame);
        } else {
            const int n = 3 * YOLO_IN * YOLO_IN;
            for (int i = 0; i < n; i++) cam_frame[i] = 0.0f;
        }
    }
    const float* input_img = g_use_camera ? cam_frame : test_image;

    int hw = YOLO_IN * YOLO_IN;
    float scale = (input_img[0] > 1.0f) ? (1.0f / 255.0f) : 1.0f;
    float32x4_t vscale = vdupq_n_f32(scale); int i = 0;
    for (; i <= hw - 4; i += 4) {
        vst1q_f32(buf_B + 0*hw + i, vmulq_f32(vld1q_f32(input_img + 0*hw + i), vscale));
        vst1q_f32(buf_B + 1*hw + i, vmulq_f32(vld1q_f32(input_img + 1*hw + i), vscale));
        vst1q_f32(buf_B + 2*hw + i, vmulq_f32(vld1q_f32(input_img + 2*hw + i), vscale));
    }
    for (; i < hw; i++) {
        buf_B[0*hw+i] = input_img[0*hw+i] * scale;
        buf_B[1*hw+i] = input_img[1*hw+i] * scale;
        buf_B[2*hw+i] = input_img[2*hw+i] * scale;
    }
    unsigned long t_rgb = get_timer_count();

    /* ── Backbone ─────────────────────────────────────────────────────── */
    LayerHandle L0 = LAYER_LOAD(ws, 16*3*3*3, 16, "L0");
    CONV2D(buf_B, YOLO_IN, YOLO_IN, 3, L0, 16, 3, 2, 1, true, buf_A);
    unsigned long t_l0 = get_timer_count();

    LayerHandle L1 = LAYER_LOAD(ws, 32*16*3*3, 32, "L1");
    CONV2D(buf_A, YOLO_S2, YOLO_S2, 16, L1, 32, 3, 2, 1, true, buf_B);
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S4, YOLO_S4, 32, 32, 1, true, ws, "L2");

    LayerHandle L3 = LAYER_LOAD(ws, 64*32*3*3, 64, "L3");
    CONV2D(buf_A, YOLO_S4, YOLO_S4, 32, L3, 64, 3, 2, 1, true, buf_B);
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S8, YOLO_S8, 64, 64, 2, true, ws, "L4");
    copy_tensor(buf_A, save_L4, 64 * YOLO_S8 * YOLO_S8);

    LayerHandle L5 = LAYER_LOAD(ws, 128*64*3*3, 128, "L5");
    CONV2D(buf_A, YOLO_S8, YOLO_S8, 64, L5, 128, 3, 2, 1, true, buf_B);
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S16, YOLO_S16, 128, 128, 2, true, ws, "L6");
    copy_tensor(buf_A, save_L6, 128 * YOLO_S16 * YOLO_S16);

    LayerHandle L7 = LAYER_LOAD(ws, 256*128*3*3, 256, "L7");
    CONV2D(buf_A, YOLO_S16, YOLO_S16, 128, L7, 256, 3, 2, 1, true, buf_B);
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S32, YOLO_S32, 256, 256, 1, true, ws, "L8");
    sppf_real_inference(buf_A, buf_B, scratch, YOLO_S32, YOLO_S32, 256, ws);
    copy_tensor(buf_B, save_SPPF, 256 * YOLO_S32 * YOLO_S32);

    unsigned long t_backbone = get_timer_count();

    /* ── Neck (PAN). v8 differs from v5: no extra 1x1 reductions between
     *    upsamples — the C2f modules handle the channel reduction. ───── */

    /* L10 upsample(SPPF) → L11 concat with L6 → L12 C2f → mid-P4 */
    upsample2x_nearest(buf_B, buf_A, YOLO_S32, YOLO_S32, 256);
    concat_tensor(buf_A, 256, save_L6, 128, scratch, YOLO_S16 * YOLO_S16);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S16, YOLO_S16, 384, 128, 1, false, ws, "L12");
    copy_tensor(buf_A, save_P4mid, 128 * YOLO_S16 * YOLO_S16);

    /* L13 upsample → L14 concat with L4 → L15 C2f → P3 head input */
    upsample2x_nearest(buf_A, buf_B, YOLO_S16, YOLO_S16, 128);
    concat_tensor(buf_B, 128, save_L4, 64, scratch, YOLO_S8 * YOLO_S8);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S8, YOLO_S8, 192, 64, 1, false, ws, "L15");
    copy_tensor(buf_A, save_P3, 64 * YOLO_S8 * YOLO_S8);

    /* L16 conv 3x3 s=2 → L17 concat with mid-P4 → L18 C2f → P4 head input */
    LayerHandle L16 = LAYER_LOAD(ws, 64*64*3*3, 64, "L16");
    CONV2D(buf_A, YOLO_S8, YOLO_S8, 64, L16, 64, 3, 2, 1, true, buf_B);
    concat_tensor(buf_B, 64, save_P4mid, 128, scratch, YOLO_S16 * YOLO_S16);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S16, YOLO_S16, 192, 128, 1, false, ws, "L18");
    copy_tensor(buf_A, save_P4, 128 * YOLO_S16 * YOLO_S16);

    /* L19 conv 3x3 s=2 → L20 concat with SPPF → L21 C2f → P5 head input */
    LayerHandle L19 = LAYER_LOAD(ws, 128*128*3*3, 128, "L19");
    CONV2D(buf_A, YOLO_S16, YOLO_S16, 128, L19, 128, 3, 2, 1, true, buf_B);
    concat_tensor(buf_B, 128, save_SPPF, 256, scratch, YOLO_S32 * YOLO_S32);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S32, YOLO_S32, 384, 256, 1, false, ws, "L21");

    unsigned long t_neck = get_timer_count();

    /* ── Detect head (anchor-free DFL).
     * Per level: cv2 (box, 64 ch) and cv3 (cls, 80 ch), each a 3-conv stack
     * (3x3 → 3x3 → 1x1). Run P5 first while buf_A still holds it. ── */

    LayerHandle wb_p3_0 = LAYER_LOAD(ws, 64*64*9,  64, "P3_BOX_0");
    LayerHandle wb_p3_1 = LAYER_LOAD(ws, 64*64*9,  64, "P3_BOX_1");
    LayerHandle wb_p3_2 = LAYER_LOAD(ws, 64*64,    64, "P3_BOX_2");
    LayerHandle wb_p4_0 = LAYER_LOAD(ws, 64*128*9, 64, "P4_BOX_0");
    LayerHandle wb_p4_1 = LAYER_LOAD(ws, 64*64*9,  64, "P4_BOX_1");
    LayerHandle wb_p4_2 = LAYER_LOAD(ws, 64*64,    64, "P4_BOX_2");
    LayerHandle wb_p5_0 = LAYER_LOAD(ws, 64*256*9, 64, "P5_BOX_0");
    LayerHandle wb_p5_1 = LAYER_LOAD(ws, 64*64*9,  64, "P5_BOX_1");
    LayerHandle wb_p5_2 = LAYER_LOAD(ws, 64*64,    64, "P5_BOX_2");

    LayerHandle wc_p3_0 = LAYER_LOAD(ws, 80*64*9,  80, "P3_CLS_0");
    LayerHandle wc_p3_1 = LAYER_LOAD(ws, 80*80*9,  80, "P3_CLS_1");
    LayerHandle wc_p3_2 = LAYER_LOAD(ws, 80*80,    80, "P3_CLS_2");
    LayerHandle wc_p4_0 = LAYER_LOAD(ws, 80*128*9, 80, "P4_CLS_0");
    LayerHandle wc_p4_1 = LAYER_LOAD(ws, 80*80*9,  80, "P4_CLS_1");
    LayerHandle wc_p4_2 = LAYER_LOAD(ws, 80*80,    80, "P4_CLS_2");
    LayerHandle wc_p5_0 = LAYER_LOAD(ws, 80*256*9, 80, "P5_CLS_0");
    LayerHandle wc_p5_1 = LAYER_LOAD(ws, 80*80*9,  80, "P5_CLS_1");
    LayerHandle wc_p5_2 = LAYER_LOAD(ws, 80*80,    80, "P5_CLS_2");

    /* P5 head — input still in buf_A. scratch is free. */
    CONV2D (buf_A,    YOLO_S32, YOLO_S32, 256, wb_p5_0, 64, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S32, YOLO_S32, 64,  wb_p5_1, 64, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S32, YOLO_S32, 64,  wb_p5_2, 64, false,       head_box);
    CONV2D (buf_A,    YOLO_S32, YOLO_S32, 256, wc_p5_0, 80, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S32, YOLO_S32, 80,  wc_p5_1, 80, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S32, YOLO_S32, 80,  wc_p5_2, 80, false,       head_cls);
    decode_v8_dfl(head_box, head_cls, YOLO_S32, YOLO_S32, 32);

    /* P4 head — input in save_P4. */
    CONV2D (save_P4,  YOLO_S16, YOLO_S16, 128, wb_p4_0, 64, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S16, YOLO_S16, 64,  wb_p4_1, 64, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S16, YOLO_S16, 64,  wb_p4_2, 64, false,       head_box);
    CONV2D (save_P4,  YOLO_S16, YOLO_S16, 128, wc_p4_0, 80, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S16, YOLO_S16, 80,  wc_p4_1, 80, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S16, YOLO_S16, 80,  wc_p4_2, 80, false,       head_cls);
    decode_v8_dfl(head_box, head_cls, YOLO_S16, YOLO_S16, 16);

    /* P3 head — input in save_P3. */
    CONV2D (save_P3,  YOLO_S8,  YOLO_S8,  64,  wb_p3_0, 64, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S8,  YOLO_S8,  64,  wb_p3_1, 64, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S8,  YOLO_S8,  64,  wb_p3_2, 64, false,       head_box);
    CONV2D (save_P3,  YOLO_S8,  YOLO_S8,  64,  wc_p3_0, 80, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S8,  YOLO_S8,  80,  wc_p3_1, 80, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S8,  YOLO_S8,  80,  wc_p3_2, 80, false,       head_cls);
    decode_v8_dfl(head_box, head_cls, YOLO_S8, YOLO_S8, 8);

    unsigned long t_nms = get_timer_count();

    if (num_preds > MAX_PREDS) num_preds = MAX_PREDS;

    for (int ii = 1; ii < num_preds; ii++) {
        Box key = preds[ii]; int jj = ii - 1;
        while (jj >= 0 && preds[jj].conf < key.conf) { preds[jj + 1] = preds[jj]; jj--; }
        preds[jj + 1] = key;
    }

    for (int ii = 0; ii < num_preds; ii++) {
        if (preds[ii].conf <= 0.0f) continue;
        for (int jj = ii + 1; jj < num_preds; jj++) {
            if (preds[jj].conf <= 0.0f) continue;
            if (preds[jj].conf < CONF_THRESH * 0.5f) break;
            if (calculate_iou(preds[ii], preds[jj]) > NMS_THRESH) preds[jj].conf = 0.0f;
        }
    }

    /* V167 layout: dark canvas (no full camera image) + bboxes + class labels.
     * Camera shown as a 192×108 thumbnail in the top-right of the canvas.
     *
     *   y=0..59   HUD top bar
     *   y=60..419 canvas (BLACK) + bboxes in CAM_DISP_* area + thumbnail
     *             on the right
     *   y=420..479 HUD bottom bar (detection cards)
     *
     * Skipping the full multi-core debayer saves ~7 ms/frame and lets the
     * thumbnail single-core (~0.5–1 ms). */
    extern void debayer_raw10_to_thumbnail(const uint8_t* raw, uint8_t* fb,
                                           uint32_t pitch, int x_off, int y_off,
                                           int thumb_w, int thumb_h);

    if (g_use_camera) {
        /* Clear the canvas region (rows 60..419) to solid black. */
        for (int y = 60; y < 420; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)lfb + (uint32_t)y * pitch);
            for (int x = 0; x < 640; x++) row[x] = 0xFF000000u;
        }
        /* Camera thumbnail in the top-right corner of the canvas (smaller). */
        const int THUMB_W = 128, THUMB_H = 72;
        const int THUMB_X = 640 - THUMB_W - 8;   /* 504 */
        const int THUMB_Y = 64;
        debayer_raw10_to_thumbnail(unicam_frame_ptr(), (uint8_t*)lfb, pitch,
                                   THUMB_X, THUMB_Y, THUMB_W, THUMB_H);
        draw_rect(THUMB_X - 1, THUMB_Y - 1, THUMB_W + 2, THUMB_H + 2,
                  0xFF00C0FFu, 1);
    } else {
        draw_fill(0xFF222222);
        draw_tensor_image_fullscreen(input_img);
    }

    /* Bbox scale: YOLO_IN²-space → CAM_DISP square at (CAM_DISP_XOFF, CAM_DISP_YOFF). */
    const float disp_scale  = g_use_camera ? (CAM_DISP_W / (float)YOLO_IN) : (640.0f / (float)YOLO_IN);
    const float disp_scaleY = g_use_camera ? (CAM_DISP_H / (float)YOLO_IN) : (480.0f / (float)YOLO_IN);
    const int   disp_xoff   = g_use_camera ? CAM_DISP_XOFF : 0;
    const int   disp_yoff   = g_use_camera ? CAM_DISP_YOFF : 0;
    const int   disp_right  = disp_xoff + (g_use_camera ? CAM_DISP_W : 640);
    const int   disp_bottom = disp_yoff + (g_use_camera ? CAM_DISP_H : 480);

    int valid_boxes = 0;

    for (int ii = 0; ii < num_preds; ii++) {
        if (preds[ii].conf > CONF_THRESH) {
            valid_boxes++;
            uart_puts("[DET] c="); uart_dec(preds[ii].cls);
            uart_puts(" %="); uart_dec((int)(preds[ii].conf * 100));
            uart_puts("\n");

            int box_w = (int)(preds[ii].w * disp_scale);
            int box_h = (int)(preds[ii].h * disp_scaleY);
            int cx    = (int)(preds[ii].x * disp_scale)  + disp_xoff;
            int cy    = (int)(preds[ii].y * disp_scaleY) + disp_yoff;

            int left = cx - (box_w / 2);
            int top  = cy - (box_h / 2);

            if (left < disp_xoff) { left = disp_xoff; }
            if (top  < disp_yoff) { top  = disp_yoff; }
            if (left + box_w > disp_right)  { box_w = disp_right  - left; }
            if (top  + box_h > disp_bottom) { box_h = disp_bottom - top; }

            if (box_w > 2 && box_h > 2) {
                static const uint32_t bbox_palette[6] = {
                    0xFF00C0FFu, 0xFF00FF80u, 0xFFFF00FFu,
                    0xFFFFC000u, 0xFF80FF00u, 0xFFFF4080u
                };
                uint32_t color = bbox_palette[((unsigned)preds[ii].cls) % 6];
                draw_rect(left, top, box_w, box_h, color, 3);

                /* Class label above the bbox: "name 67%" */
                const char* nm = (preds[ii].cls >= 0 && preds[ii].cls < 80)
                                 ? coco_names[preds[ii].cls] : "?";
                int pct = (int)(preds[ii].conf * 100.0f);
                if (pct > 99) pct = 99;
                char lbl[40]; int ln = 0;
                while (*nm && ln < 30) lbl[ln++] = *nm++;
                lbl[ln++] = ' ';
                if (pct >= 10) lbl[ln++] = '0' + (pct / 10);
                lbl[ln++] = '0' + (pct % 10);
                lbl[ln++] = '%';
                lbl[ln] = '\0';
                int label_y = (top - 18 >= disp_yoff) ? top - 18 : top + 4;
                draw_text(left + 2, label_y, lbl, color, 0xFF000000u, 2);
            }
        }
    }
    if (valid_boxes == 0) uart_puts("[DET] none\n");
    heartbeat_counter++;

    unsigned long t_end = get_timer_count();

    /* B0: per-stage profiling. cap → rgb prep, l0 → first conv2d,
     * backbone → through L8, neck → through L23, head → up to NMS,
     * total → end. */
    {
        unsigned long ms_cap      = (t_rgb      - t_start)    * 1000UL / f;
        unsigned long ms_l0       = (t_l0       - t_rgb)      * 1000UL / f;
        unsigned long ms_backbone = (t_backbone - t_l0)       * 1000UL / f;
        unsigned long ms_neck     = (t_neck     - t_backbone) * 1000UL / f;
        unsigned long ms_head     = (t_nms      - t_neck)     * 1000UL / f;
        unsigned long ms_render   = (t_end      - t_nms)      * 1000UL / f;
        uart_puts("[P] cap=");     uart_dec((int)ms_cap);
        uart_puts(" l0=");         uart_dec((int)ms_l0);
        uart_puts(" bb=");         uart_dec((int)ms_backbone);
        uart_puts(" neck=");       uart_dec((int)ms_neck);
        uart_puts(" head=");       uart_dec((int)ms_head);
        uart_puts(" render=");     uart_dec((int)ms_render);
        uart_puts("ms\n");
    }

    /* HUD overlay — repaint the top + bottom letterbox bars with KPI chrome,
     * detection cards, animated FPS gauge. Camera area (rows 60..419) is
     * untouched. */
    {
        unsigned long duration_ms = (t_end - t_start) * 1000UL / f;
        HudPred hud_preds[3]; int hud_n = 0;
        for (int ii = 0; ii < num_preds && hud_n < 3; ii++) {
            if (preds[ii].conf > CONF_THRESH) {
                hud_preds[hud_n].cls  = preds[ii].cls;
                hud_preds[hud_n].conf = preds[ii].conf;
                hud_n++;
            }
        }
        hud_update_state(hud_preds, hud_n, global_frame_counter,
                         (int)duration_ms, (int)imx708_ae_cit_get());
        hud_render((uint8_t*)lfb, pitch);
    }

    num_preds = 0; video_flush();

    uart_puts("[T] "); uart_dec((t_end-t_start)*1000/f); uart_puts("ms\n");
}
