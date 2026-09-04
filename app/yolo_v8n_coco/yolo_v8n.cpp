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

#ifdef SERIAL_BOOT
/* V183 serial-boot bench (tools/hwbench.py): the ~60 KB code-only kernel is
 * streamed over UART and data.s is NOT linked. The FP32 weights + test image
 * ride the SD as d2m_data.bin (tools/pack_data.py), loaded by the VPU at
 * D2M_BLOB_ADDR via `initramfs` in build/config.serial.txt. Layout:
 *   [weights.bin (WEIGHTS_SIZE)] [pad to 16] [test_image.bin]
 * The bench is fp32-only — the INT8 blobs are not packed. */
#if defined(USE_INT8_W8A8) || defined(USE_INT8_WEIGHTS)
#error "SERIAL_BOOT is fp32-only: build without USE_INT8 / USE_INT8_W8A8"
#endif
#define D2M_BLOB_ADDR 0x08000000UL
static const float*  const weights_start          = (const float*)D2M_BLOB_ADDR;
static const float*  const weights_end            = (const float*)(D2M_BLOB_ADDR + WEIGHTS_SIZE);
static const float*  const test_image             = (const float*)(D2M_BLOB_ADDR + ((WEIGHTS_SIZE + 15UL) & ~15UL));
static const int8_t* const weights_int8_start      = nullptr;   /* unused: fp32 bench */
static const int8_t* const weights_int8_end        = nullptr;
static const int8_t* const weights_int8_w8a8_start = nullptr;
static const int8_t* const weights_int8_w8a8_end   = nullptr;
#else
/* Weight blobs + test image — defined by app/yolo_v8n_coco/data.s via .incbin. */
extern "C" const float  weights_start[];
extern "C" const float  weights_end[];
extern "C" const int8_t weights_int8_start[];        /* Tier 1 — unused outside USE_INT8_WEIGHTS */
extern "C" const int8_t weights_int8_end[];
extern "C" const int8_t weights_int8_w8a8_start[];   /* Tier 2 — unused outside USE_INT8_W8A8  */
extern "C" const int8_t weights_int8_w8a8_end[];
extern "C" const float  test_image[];
#endif

/* V192: non-square input (full sensor FoV). Per-stride grid dims, H and W
 * separate. YOLO_W/YOLO_H come from bsp.h; both are multiples of 32. */
#define YOLO_S2H  (YOLO_H / 2)
#define YOLO_S2W  (YOLO_W / 2)
#define YOLO_S4H  (YOLO_H / 4)
#define YOLO_S4W  (YOLO_W / 4)
#define YOLO_S8H  (YOLO_H / 8)
#define YOLO_S8W  (YOLO_W / 8)
#define YOLO_S16H (YOLO_H / 16)
#define YOLO_S16W (YOLO_W / 16)
#define YOLO_S32H (YOLO_H / 32)
#define YOLO_S32W (YOLO_W / 32)

#define DFL_REG_MAX 16   /* v8 default; box ch = 4 * REG_MAX */

/* Activation element type. W8A8 makes all inter-layer buffers int8; the
 * model graph's bare-metal control flow is otherwise identical. */
#ifdef USE_INT8_W8A8
typedef int8_t act_t;
#else
typedef float  act_t;
#endif

static act_t buf_A[2000000]; static act_t buf_B[2000000]; static act_t scratch[2000000];
static float cam_frame[3 * YOLO_W * YOLO_H];   /* fp32 — debayer/preprocess input */

/* Skip connections + per-level head inputs (v8 PAN). */
static act_t save_L4   [64  * YOLO_S8H * YOLO_S8W];    /* L4 (P3 backbone)     */
static act_t save_L6   [128 * YOLO_S16H * YOLO_S16W];   /* L6 (P4 backbone)     */
static act_t save_SPPF [256 * YOLO_S32H * YOLO_S32W];   /* L9 SPPF out          */
static act_t save_P4mid[128 * YOLO_S16H * YOLO_S16W];   /* L12 (mid neck)       */
static act_t save_P3   [64  * YOLO_S8H * YOLO_S8W];    /* L15 → P3 head input  */
static act_t save_P4   [128 * YOLO_S16H * YOLO_S16W];   /* L18 → P4 head input  */

/* Head per-level scratch. Sized for P3 (largest spatial grid). */
static act_t head_box [64 * YOLO_S8H * YOLO_S8W];
static act_t head_cls [80 * YOLO_S8H * YOLO_S8W];
static act_t head_tmp [80 * YOLO_S8H * YOLO_S8W];

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
[[maybe_unused]]
static void copy_tensor(const float* src, float* dst, int n) {
    int i = 0; for (; i <= n - 4; i += 4) vst1q_f32(dst + i, vld1q_f32(src + i));
    for (; i < n; i++) dst[i] = src[i];
}
[[maybe_unused]]
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

/* Forward decl — decode_v8_dfl is defined further down the file. */
static void decode_v8_dfl(const float* box_chw, const float* cls_chw,
                          int gh, int gw, int stride);

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

/* Peek L0's scale_in from the W8A8 bin without consuming the stream.
 * Bin layout starts with: uint32 n_w + float scale_w + float scale_in, so
 * scale_in is at byte offset 8. Used by the input preprocess to derive
 * the fp32→int8 quantization factor (rather than hardcoding 1/127),
 * which makes the preprocess auto-track any `--scale_safety_margin`
 * applied in tools/calibrate_int8.py. */
[[maybe_unused]]
static inline float w8a8_l0_scale_in() {
    return *((const float*)(weights_int8_w8a8_start + 8));
}

/* Dequant the int8 head outputs (box + cls) into a fp32 scratch buffer,
 * then run the existing fp32 DFL decoder. The scratch buffer is the
 * model's `scratch` array reinterpreted — it's int8 in W8A8 mode but
 * holds 2 M bytes = 500 K fp32 elements, more than enough for the
 * largest head grid (P3 = 144 × 1024 = 147 456 fp32). */
[[maybe_unused]]
static void w8a8_decode_head(const int8_t* head_box_i8, const int8_t* head_cls_i8,
                             int gh, int gw, int stride,
                             float box_scale, float cls_scale,
                             float* scratch_fp) {
    int grd = gh * gw;
    float* hb_fp = scratch_fp;
    float* hc_fp = scratch_fp + 64 * grd;
    dequant_int8_to_fp32_neon(head_box_i8, 64 * grd, box_scale, hb_fp);
    dequant_int8_to_fp32_neon(head_cls_i8, 80 * grd, cls_scale, hc_fp);
    decode_v8_dfl(hb_fp, hc_fp, gh, gw, stride);
}

/* ──────────────────────────────────────────────────────────────────────────
 * G2 Tier 2 — W8A8 debug instrumentation. Built with
 *   make USE_INT8_W8A8=1 W8A8_DEBUG=1
 * Prints `[ABS] tag i8=AMAX rt1e3=RT cal1e3=CAL` after each checkpoint,
 * where AMAX = max|int8| in the output (0..127), RT = AMAX × scale_out
 * × 1000 (runtime fp32 max in thousandths), CAL = 127 × scale_out × 1000
 * (the absmax that calibration saw, in thousandths). Comparing AMAX
 * against 127 and RT against CAL reveals dynamic-range underuse or
 * activation overshoot vs the offline calibration.
 * ────────────────────────────────────────────────────────────────────── */
#if defined(W8A8_DEBUG) && defined(USE_INT8_W8A8)
static int w8a8_absmax_i8(const int8_t* buf, int n) {
    int8x16_t v_max = vdupq_n_s8(0);
    int i = 0;
    for (; i <= n - 16; i += 16) {
        int8x16_t v     = vld1q_s8(buf + i);
        int8x16_t v_neg = vqnegq_s8(v);              /* saturated negate */
        int8x16_t v_abs = vmaxq_s8(v, v_neg);
        v_max = vmaxq_s8(v_max, v_abs);
    }
    int8_t lanes[16];
    vst1q_s8(lanes, v_max);
    int mx = 0;
    for (int j = 0; j < 16; j++) if (lanes[j] > mx) mx = lanes[j];
    for (; i < n; i++) {
        int a = buf[i] < 0 ? -(int)buf[i] : (int)buf[i];
        if (a > mx) mx = a;
    }
    return mx;
}

static void w8a8_dbg_log_output(const char* tag, const int8_t* buf, int n, float scale_out) {
    int amax = w8a8_absmax_i8(buf, n);
    int rt_milli  = (int)((float)amax * scale_out * 1000.0f + 0.5f);
    int cal_milli = (int)(127.0f      * scale_out * 1000.0f + 0.5f);
    uart_puts("[ABS] "); uart_puts(tag);
    uart_puts(" i8=");      uart_dec(amax);
    uart_puts(" rt1e3=");   uart_dec(rt_milli);
    uart_puts(" cal1e3=");  uart_dec(cal_milli);
    uart_puts("\n");
}
#define LOG_ABSMAX(tag, buf, n, L) w8a8_dbg_log_output((tag), (buf), (n), (L).scale_out)
#elif defined(SERIAL_BOOT)
/* V183 bench build (fp32): the same checkpoints print a numeric fingerprint
 * `[ABS] tag amax1e3=N` (max|x| × 1000 as int). tools/hwbench.py diffs these
 * against GOLDEN_ABS — a per-layer correctness check that works even when
 * test_image yields no detections above CONF_THRESH. Same binary in QEMU and
 * on HW → values should match to the thousandth. */
static void fp32_dbg_log_output(const char* tag, const float* buf, int n) {
    float32x4_t v_max = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i <= n - 4; i += 4) v_max = vmaxq_f32(v_max, vabsq_f32(vld1q_f32(buf + i)));
    float lanes[4]; vst1q_f32(lanes, v_max);
    float mx = lanes[0];
    for (int j = 1; j < 4; j++) if (lanes[j] > mx) mx = lanes[j];
    for (; i < n; i++) { float a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > mx) mx = a; }
    uart_puts("[ABS] "); uart_puts(tag);
    uart_puts(" amax1e3="); uart_dec((int)(mx * 1000.0f + 0.5f));
    uart_puts("\n");
}
#define LOG_ABSMAX(tag, buf, n, L) fp32_dbg_log_output((tag), (buf), (n))
#else
#define LOG_ABSMAX(tag, buf, n, L) ((void)0)
#endif

/* FP32 NEON residual add helper — extracted from c2f_real_inference so
 * the macro path can use it uniformly. */
[[maybe_unused]]
static inline void fp32_residual_add_neon(float* x_out, const float* x_in, int n) {
    int k = 0;
    for (; k <= n - 4; k += 4) {
        float32x4_t a = vld1q_f32(x_in  + k);
        float32x4_t v = vld1q_f32(x_out + k);
        vst1q_f32(x_out + k, vaddq_f32(a, v));
    }
    for (; k < n; k++) x_out[k] += x_in[k];
}

/* Forward decls — predictions + frame counter live further down the file
 * but display_pump (below) reads them while iterating. Their bodies stay
 * in their original locations so the model code reads top-to-bottom. */
struct Box { float x, y, w, h, conf; int cls; };
static Box preds[MAX_PREDS];
static int num_preds = 0;
static int global_frame_counter = 1;

/* V181 tracker storage — Track struct + tracks[] live here so display_pump
 * (HUD card collection) can see them. update_tracker() body is further down
 * with the rest of the post-NMS code; it needs calculate_iou which is
 * declared after this block.
 *
 * V182: added linear velocity prediction. Each track remembers its last
 * inter-inference displacement (vx, vy). On the next round, the predicted
 * bbox = current + (vx, vy); matching uses IoU against the predicted box
 * rather than the stale one. Robust to a walking subject at V180's ~1 fps
 * inference cadence (a person can easily traverse > 30 % bbox width in
 * one second, defeating a plain IoU tracker). On miss frames the bbox is
 * advanced by velocity too, so the rendered bbox glides while inference
 * catches up. Velocity is clamped to TRACK_V_MAX per axis to prevent a
 * runaway after a bad observation. */
#define MAX_TRACKS         16
#define TRACK_IOU_MATCH    0.30f
#define TRACK_MAX_MISSED   5
#define TRACK_V_MAX        80.0f   /* ~ YOLO_W / 4 */
struct Track {
    int   id;              /* 0 = free slot */
    Box   box;             /* cls + conf + x/y/w/h in YOLO_W x YOLO_H space */
    float vx, vy;          /* per-inference velocity */
    int   frames_missed;
    bool  has_velocity;    /* false until first successful re-match */
};
static Track tracks[MAX_TRACKS];
static int   next_track_id = 1;
static void  update_tracker();

/* ──────────────────────────────────────────────────────────────────────────
 * V180 — Async inference + display pump. Default FP32 build only.
 *
 * Cores 1-3 run the conv (3-way split, n_grp/3 per worker) while core 0
 * services the camera + thumbnail + HUD. Each conv dispatch is wrapped
 * by `parallel_conv2d_with_pump` which kicks the workers and then loops:
 *   while (!done) display_pump();
 *
 * `display_pump` consumes one camera frame (bsp_frame_acquire waits the
 * next FSI + runs AE), redraws the thumbnail + HUD, flushes the FB.
 * Cost: ~30-40 ms per pump (FSI wait dominates). Cores 1-3's per-conv
 * work ranges from <1 ms to ~50 ms; pumps may overshoot the conv's
 * completion (poll returns ready mid-pump and the next loop iteration
 * just exits). Inference wall-clock grows ~33 % vs the 4-core sync
 * path (3 workers instead of 4); in exchange the display refreshes
 * continuously instead of freezing for ~720 ms per inference.
 * ────────────────────────────────────────────────────────────────────── */

#if !defined(USE_INT8_W8A8) && !defined(USE_INT8_WEIGHTS)
extern void debayer_raw10_to_thumbnail(const uint8_t* raw, uint8_t* fb,
                                       uint32_t pitch, int x_off, int y_off,
                                       int thumb_w, int thumb_h);

static void display_pump() {
    /* Drive the Unicam ping-pong state machine non-blockingly. When a
     * fresh frame just landed, refresh the on-screen chrome; otherwise
     * we just advanced one state (~one MMIO read) and return so the
     * conv-done check stays tight. AE is intentionally NOT run here —
     * the outer bsp_frame_acquire (once per inference) owns AE. */
    if (!g_use_camera) return;
    if (!bsp_frame_try_advance()) return;

#if DEBUG
    /* Debug build only: repaint camera thumbnail with the fresh frame. */
    const uint8_t* now = unicam_frame_ptr();
    if (now != NULL) {
        const int THUMB_W = 128, THUMB_H = 72;
        const int THUMB_X = 640 - THUMB_W - 8;
        const int THUMB_Y = 64;
        debayer_raw10_to_thumbnail(now, (uint8_t*)lfb, pitch,
                                   THUMB_X, THUMB_Y, THUMB_W, THUMB_H);
    }
#endif

    /* Refresh HUD chrome — fps/CIT gauge animates while inference runs.
     * Cards come from active tracks so they stay consistent with bboxes
     * even though preds[] gets overwritten between inferences.
     * duration_ms=0 keeps the gauge bar at zero during pumps; the outer
     * frame's [T] update at end-of-inference refills it. */
    HudPred hud_preds[3]; int hud_n = 0;
    float   hud_conf[3];
    for (int ti = 0; ti < MAX_TRACKS; ti++) {
        if (tracks[ti].id == 0) continue;
        const Box& b = tracks[ti].box;
        if (b.conf <= CONF_THRESH) continue;
        int slot = hud_n;
        if (hud_n < 3) hud_n++;
        else if (b.conf > hud_conf[2]) slot = 2;
        else continue;
        while (slot > 0 && b.conf > hud_conf[slot-1]) {
            hud_conf[slot]  = hud_conf[slot-1];
            hud_preds[slot] = hud_preds[slot-1];
            slot--;
        }
        hud_conf[slot]       = b.conf;
        hud_preds[slot].cls  = b.cls;
        hud_preds[slot].conf = b.conf;
    }
    hud_update_state(hud_preds, hud_n, global_frame_counter,
                     0, (int)imx708_ae_cit_get());
    hud_render((uint8_t*)lfb, pitch);
    video_flush();
}

/* Async wait body — used by both conv2d and conv1x1 wrappers. Spin pumping
 * the display while workers run. The wfe between pumps puts core 0 to
 * sleep until the workers' sev when they increment done_count — under
 * QEMU this is essential (yield alone doesn't reschedule cores aggressively
 * enough and core 0 starves the workers). On real HW wfe is also cheaper
 * than tight spinning. */
static inline void async_wait_with_pump() {
    while (!parallel_async_done()) {
        display_pump();
        if (parallel_async_done()) break;
        asm volatile("wfe");
    }
    parallel_async_wait();
}

static inline void parallel_conv2d_with_pump(const float* in, int H, int W, int C_in,
                                             const float* w_rep, const float* bias,
                                             int C_out, int K, int stride, int pad,
                                             bool do_silu, float* out) {
    parallel_conv2d_async_start(in, H, W, C_in, w_rep, bias, C_out, K, stride, pad, do_silu, out);
    async_wait_with_pump();
}

static inline void parallel_conv1x1_with_pump(const float* in, int H, int W, int C_in,
                                              const float* w, const float* b, int C_out,
                                              bool do_silu, float* out) {
    parallel_conv1x1_async_start(in, H, W, C_in, w, b, C_out, do_silu, out);
    async_wait_with_pump();
}
#endif  /* !USE_INT8_W8A8 && !USE_INT8_WEIGHTS */


/* ──────────────────────────────────────────────────────────────────────────
 * Unified layer handle + dispatch macros that select the model precision
 * at compile time. Three modes:
 *   USE_INT8_W8A8     (Tier 2) — int8 weights + int8 activations
 *   USE_INT8_WEIGHTS  (Tier 1) — int8 weights, fp32 activations
 *   (default)                  — fp32 throughout
 * Same run_yolo_complete / c2f / sppf body services all three.
 * ────────────────────────────────────────────────────────────────────── */
#ifdef USE_INT8_W8A8

typedef WeightStreamW8A8::Layer LayerHandle;

#define WS_TYPE       WeightStreamW8A8
#define WS_INIT(name) WeightStreamW8A8 name(weights_int8_w8a8_start)
#define LAYER_LOAD(ws, nw, nb, tag) (ws).next((nw), (nb), (tag))
#define CONV2D(in, H, W, ci, L, co, K, s, p, silu, out) \
    w8a8_conv2d_dispatch((in), (H), (W), (ci), (L), (co), (K), (s), (p), (silu), (out))
#define CONV1X1(in, H, W, ci, L, co, silu, out) \
    conv1x1_int8((in), (H), (W), (ci), (L).weights, (L).bias, \
                 (L).scale_w, (L).scale_in, (L).scale_out, (co), (silu), (out))
#define COPY_TENSOR(src, dst, n)               copy_tensor_i8((src), (dst), (n))
#define CONCAT_TENSOR(s1, c1, s2, c2, dst, hw) concat_tensor_i8((s1), (c1), (s2), (c2), (dst), (hw))
#define UPSAMPLE2X(in, out, H, W, C)           upsample2x_nearest_i8((in), (out), (H), (W), (C))
#define MAXPOOL5X5(in, out, H, W, C)           maxpool5x5_s1_p2_i8((in), (out), (H), (W), (C))
/* Residual: x_out at L_out's scale_out, x_in at L_in's scale_out (the
 * conv that wrote the previous slot in c2f's temp ring). */
#define RESIDUAL_ADD(x_out, x_in, n, L_out, L_in) \
    w8a8_residual_add((x_out), (x_in), (n), (L_out).scale_out, (L_in).scale_out)
#define DECODE_HEAD(hb, hc, gh, gw, stride, L_box, L_cls) \
    w8a8_decode_head((hb), (hc), (gh), (gw), (stride), \
                     (L_box).scale_out, (L_cls).scale_out, (float*)scratch)

#elif defined(USE_INT8_WEIGHTS)

/* Tier 1 W8A32. Dequant scratch sized for the largest layer in
 * YOLOv8n@256 (L7 = 256*128*9 = 294 912 floats). */
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
#define COPY_TENSOR(src, dst, n)               copy_tensor((src), (dst), (n))
#define CONCAT_TENSOR(s1, c1, s2, c2, dst, hw) concat_tensor((s1), (c1), (s2), (c2), (dst), (hw))
#define UPSAMPLE2X(in, out, H, W, C)           upsample2x_nearest((in), (out), (H), (W), (C))
#define MAXPOOL5X5(in, out, H, W, C)           maxpool5x5_s1_p2((in), (out), (H), (W), (C))
#define RESIDUAL_ADD(x_out, x_in, n, L_out, L_in) \
    fp32_residual_add_neon((x_out), (x_in), (n))
#define DECODE_HEAD(hb, hc, gh, gw, stride, L_box, L_cls) \
    decode_v8_dfl((hb), (hc), (gh), (gw), (stride))

#else

/* Pure FP32. LayerHandle bundles the two ws.next() results so c2f and
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
/* V180: FP32 path uses async dispatch + display pump on HW so core 0 keeps
 * the camera + HUD alive while cores 1-3 grind the conv. ~33% more
 * wall-clock inference vs sync 4-core, but no display freeze. Gated on
 * !SIMULATION because QEMU time-shares the 4 emulated cores on one host
 * CPU — core 0 spinning + pumping starves the workers and one frame
 * stretches to 80 s. Sim builds stay on the sync 4-core path. */
#ifdef SIMULATION
#define CONV2D(in, H, W, ci, L, co, K, s, p, silu, out) \
    parallel_conv2d((in), (H), (W), (ci), (L).w, (L).b, (co), (K), (s), (p), (silu), (out))
#define CONV1X1(in, H, W, ci, L, co, silu, out) \
    parallel_conv1x1((in), (H), (W), (ci), (L).w, (L).b, (co), (silu), (out))
#else
#define CONV2D(in, H, W, ci, L, co, K, s, p, silu, out) \
    parallel_conv2d_with_pump((in), (H), (W), (ci), (L).w, (L).b, (co), (K), (s), (p), (silu), (out))
#define CONV1X1(in, H, W, ci, L, co, silu, out) \
    parallel_conv1x1_with_pump((in), (H), (W), (ci), (L).w, (L).b, (co), (silu), (out))
#endif
#define COPY_TENSOR(src, dst, n)               copy_tensor((src), (dst), (n))
#define CONCAT_TENSOR(s1, c1, s2, c2, dst, hw) concat_tensor((s1), (c1), (s2), (c2), (dst), (hw))
#define UPSAMPLE2X(in, out, H, W, C)           upsample2x_nearest((in), (out), (H), (W), (C))
#define MAXPOOL5X5(in, out, H, W, C)           maxpool5x5_s1_p2((in), (out), (H), (W), (C))
#define RESIDUAL_ADD(x_out, x_in, n, L_out, L_in) \
    fp32_residual_add_neon((x_out), (x_in), (n))
#define DECODE_HEAD(hb, hc, gh, gw, stride, L_box, L_cls) \
    decode_v8_dfl((hb), (hc), (gh), (gw), (stride))

#endif

static float calculate_iou(const Box& a, const Box& b) {
    float x1_int = (a.x - a.w/2) > (b.x - b.w/2) ? (a.x - a.w/2) : (b.x - b.w/2);
    float y1_int = (a.y - a.h/2) > (b.y - b.h/2) ? (a.y - a.h/2) : (b.y - b.h/2);
    float x2_int = (a.x + a.w/2) < (b.x + b.w/2) ? (a.x + a.w/2) : (b.x + b.w/2);
    float y2_int = (a.y + a.h/2) < (b.y + b.h/2) ? (a.y + a.h/2) : (b.y + b.h/2);
    float w_int = x2_int - x1_int; float h_int = y2_int - y1_int;
    if (w_int <= 0 || h_int <= 0) return 0.0f;
    float area_int = w_int * h_int; return area_int / (a.w * a.h + b.w * b.h - area_int);
}

/* ── V181: greedy IoU tracker ────────────────────────────────────────────
 * Assigns a stable integer ID to each detection across inferences. On every
 * inference (post-NMS):
 *   1. Each active track greedily binds to the unused pred of the same
 *      class with highest IoU above TRACK_IOU_MATCH.
 *   2. Bound preds are consumed; track bbox + conf are refreshed.
 *   3. Tracks without a match this round get frames_missed++; expired
 *      after TRACK_MAX_MISSED rounds (~5 inferences ≈ 5 s @ V180 cadence).
 *   4. Unmatched preds become brand-new tracks with the next free ID.
 * No persistence filter — every track renders the same frame it's born
 * (so first appearance has no extra latency). Storage (Track struct +
 * tracks[]) is declared near the top of this file so display_pump can
 * iterate it. */
static void update_tracker() {
    bool pred_used[MAX_PREDS];
    for (int i = 0; i < MAX_PREDS; i++) pred_used[i] = false;

    /* 1) Refresh / age existing tracks, matching against the predicted
     *    bbox rather than the stale one. */
    for (int t = 0; t < MAX_TRACKS; t++) {
        if (tracks[t].id == 0) continue;

        /* Linear extrapolation: predicted box = current + last velocity.
         * First round after spawn (has_velocity == false) just uses the
         * current box, identical to V181. */
        Box pred = tracks[t].box;
        if (tracks[t].has_velocity) {
            pred.x += tracks[t].vx;
            pred.y += tracks[t].vy;
        }

        float best_iou = TRACK_IOU_MATCH;
        int   best_p   = -1;
        for (int p = 0; p < num_preds; p++) {
            if (pred_used[p]) continue;
            if (preds[p].conf <= CONF_THRESH) continue;
            if (preds[p].cls != tracks[t].box.cls) continue;
            float iou = calculate_iou(pred, preds[p]);
            if (iou > best_iou) { best_iou = iou; best_p = p; }
        }
        if (best_p >= 0) {
            /* New velocity = actual displacement since last observation,
             * clamped per axis so a single bad observation doesn't send
             * the predictor off-screen on subsequent rounds. */
            float nvx = preds[best_p].x - tracks[t].box.x;
            float nvy = preds[best_p].y - tracks[t].box.y;
            if (nvx >  TRACK_V_MAX) nvx =  TRACK_V_MAX;
            if (nvx < -TRACK_V_MAX) nvx = -TRACK_V_MAX;
            if (nvy >  TRACK_V_MAX) nvy =  TRACK_V_MAX;
            if (nvy < -TRACK_V_MAX) nvy = -TRACK_V_MAX;
            tracks[t].vx           = nvx;
            tracks[t].vy           = nvy;
            tracks[t].has_velocity = true;
            tracks[t].box          = preds[best_p];
            tracks[t].frames_missed = 0;
            pred_used[best_p]      = true;
        } else {
            /* Coast: glide the bbox forward so the rendered overlay stays
             * with a moving subject while inference catches up, and so
             * next round's prediction starts from the extrapolated point
             * instead of the stale anchor. */
            if (tracks[t].has_velocity) {
                tracks[t].box.x += tracks[t].vx;
                tracks[t].box.y += tracks[t].vy;
            }
            tracks[t].frames_missed++;
            if (tracks[t].frames_missed > TRACK_MAX_MISSED) tracks[t].id = 0;
        }
    }

    /* 2) Spawn new tracks for unmatched preds. */
    for (int p = 0; p < num_preds; p++) {
        if (pred_used[p]) continue;
        if (preds[p].conf <= CONF_THRESH) continue;
        for (int t = 0; t < MAX_TRACKS; t++) {
            if (tracks[t].id == 0) {
                tracks[t].id            = next_track_id++;
                if (next_track_id > 9999) next_track_id = 1;
                tracks[t].box           = preds[p];
                tracks[t].vx            = 0.0f;
                tracks[t].vy            = 0.0f;
                tracks[t].has_velocity  = false;
                tracks[t].frames_missed = 0;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * YOLOv8 anchor-free DFL decoder.
 *
 * Reads the per-level box (64ch, CHW) and class (80ch, CHW, raw logits)
 * grids produced by Detect.cv2[i] and Detect.cv3[i]. For every cell we:
 *   1. find argmax over 80 raw cls logits, sigmoid it, prefilter.
 *   2. for each of 4 sides (l, t, r, b), softmax over 16 bins and take
 *      the integral E[k] = Σ k * softmax(box[s*16:(s+1)*16]).
 *   3. recover (x, y, w, h) in YOLO_W x YOLO_H space using anchor at cell centre.
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
void c2f_real_inference(act_t* in, act_t* out, act_t* temp,
                        int h, int w, int c_in, int c_out, int n_depth,
                        bool shortcut, WS_TYPE& ws, const char* prefix) {
    int c_hidden = c_out / 2;
    int hw = h * w;
    int hw_h = hw * c_hidden;
    int c_concat = (2 + n_depth) * c_hidden;

    /* V183: breakdown profiling for a single C2f instance. `make C2F_PROF=N`
     * (default 6) picks which one; the marks land between that layer's outer
     * mark and the previous one, so they sum to its bucket. */
#ifndef C2F_PROF_LAYER
#define C2F_PROF_LAYER 6
#endif
    int c2f_num = 0;
    for (const char* pc = prefix + 1; *pc >= '0' && *pc <= '9'; pc++) c2f_num = c2f_num * 10 + (*pc - '0');
    const bool CP = (prefix[0] == 'L' && c2f_num == (C2F_PROF_LAYER));
    #define C2F_MARK(s) do { if (CP) prof_mark(s); } while (0)

    LayerHandle Lcv1 = LAYER_LOAD(ws, c_in * (2 * c_hidden), 2 * c_hidden, "C2f_CV1");
    LayerHandle Lcv2 = LAYER_LOAD(ws, c_concat * c_out,      c_out,        "C2f_CV2");

    /* cv1: writes (a | b) contiguously into temp slots 0 and 1. */
    CONV1X1(in, h, w, c_in, Lcv1, 2 * c_hidden, true, temp);                              C2F_MARK("  C2f.cv1 1x1");

    /* L_prev_out tracks the layer that last wrote into the slot we're
     * about to consume as x_in. For i==0 that slot is the second half
     * of cv1's output, so its scale is Lcv1.scale_out. For i>0 it's the
     * x_out written by the previous iteration (= previous Lb2). Only
     * matters for the W8A8 residual; FP32 ignores the layer args. */
    LayerHandle L_prev_out = Lcv1;

    for (int i = 0; i < n_depth; i++) {
        LayerHandle Lb1 = LAYER_LOAD(ws, c_hidden * c_hidden * 9, c_hidden, "Bot_CV1");
        LayerHandle Lb2 = LAYER_LOAD(ws, c_hidden * c_hidden * 9, c_hidden, "Bot_CV2");

        act_t* x_in  = temp + (1 + i) * hw_h;          /* previous slot */
        act_t* x_mid = out;                            /* scratch */
        act_t* x_out = temp + (2 + i) * hw_h;          /* new slot */

        CONV2D(x_in,  h, w, c_hidden, Lb1, c_hidden, 3, 1, 1, true, x_mid);              C2F_MARK("  C2f.bot cv1 3x3");
        CONV2D(x_mid, h, w, c_hidden, Lb2, c_hidden, 3, 1, 1, true, x_out);              C2F_MARK("  C2f.bot cv2 3x3");

        if (shortcut) {
            RESIDUAL_ADD(x_out, x_in, hw_h, Lb2, L_prev_out);
        }
        L_prev_out = Lb2;                                                                 C2F_MARK("  C2f.bot residual");
    }

    CONV1X1(temp, h, w, c_concat, Lcv2, c_out, true, out);                                C2F_MARK("  C2f.cv2 1x1");
    LOG_ABSMAX(prefix, out, c_out * hw, Lcv2);
    #undef C2F_MARK
}

void sppf_real_inference(act_t* in, act_t* out, act_t* temp,
                         int h, int w, int c, WS_TYPE& ws) {
    int c_hidden = c / 2; int hw = h * w; int hw_hidden = c_hidden * hw;
    LayerHandle L1 = LAYER_LOAD(ws, c * c_hidden,            c_hidden, "SPPF_1");
    LayerHandle L2 = LAYER_LOAD(ws, (c_hidden * 4) * c,      c,        "SPPF_2");

    act_t* cv1_out = temp; CONV1X1(in, h, w, c, L1, c_hidden, true, cv1_out);
    act_t* m1 = temp + hw_hidden; act_t* m2 = temp + 2*hw_hidden; act_t* m3 = temp + 3*hw_hidden;

    MAXPOOL5X5(cv1_out, m1, h, w, c_hidden);
    MAXPOOL5X5(m1, m2, h, w, c_hidden);
    MAXPOOL5X5(m2, m3, h, w, c_hidden);
    CONV1X1(temp, h, w, c_hidden * 4, L2, c, true, out);
    LOG_ABSMAX("SPPF", out, c * hw, L2);
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
    const float* dst_g = img + (YOLO_W * YOLO_H);
    const float* dst_b = img + (2 * YOLO_W * YOLO_H);
    for (int y = 0; y < 480; y++) {
        int src_y = (y * YOLO_H) / 480;
        for (int x = 0; x < 640; x++) {
            int src_x = (x * YOLO_W) / 640;
            int idx = src_y * YOLO_W + src_x;
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

#ifdef CAPTURE_FRAME
/* ── V185: frame capture over UART ─────────────────────────────────────────
 * `make capture` (SERIAL_BOOT + CAPTURE_FRAME=N, camera ON). On frame N — by
 * then AE has settled — dump cam_frame, the *exact* [3][YOLO_H][YOLO_W] fp32
 * tensor the model consumes after debayer + ISP, as base64 between
 *   [CAP] begin len=<bytes> crc=<hex32>   …   [CAP] end
 * hwbench.py --capture reassembles + CRC-checks it; tools/capture_to_test_image.py
 * installs it as app/<APP>/test_image.bin. ~1 MB at 115200 ≈ 90 s, so the
 * watchdog is kicked every few lines. */
static void uart_hex32(uint32_t v) {
    static const char H[] = "0123456789abcdef";
    char s[9]; for (int i = 7; i >= 0; i--) { s[i] = H[v & 15]; v >>= 4; } s[8] = 0;
    uart_puts(s);
}
static void uart_b64(const uint8_t* p, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80]; int li = 0, lines = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((i + 1 < n ? (uint32_t)p[i+1] : 0u) << 8) | (i + 2 < n ? (uint32_t)p[i+2] : 0u);
        line[li++] = T[(v >> 18) & 63]; line[li++] = T[(v >> 12) & 63];
        line[li++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        line[li++] = (i + 2 < n) ? T[v & 63] : '=';
        if (li >= 76) { line[li] = 0; uart_puts(line); uart_puts("\n"); li = 0;
                        if ((++lines & 31) == 0) watchdog_kick(); }
    }
    if (li) { line[li] = 0; uart_puts(line); uart_puts("\n"); }
    watchdog_kick();
}
static void capture_dump_if_due(const float* img) {
    static bool done = false;
    /* global_frame_counter is already post-incremented for this frame. */
    if (done || global_frame_counter != (CAPTURE_FRAME)) return;
    done = true;
    const uint8_t* p = (const uint8_t*)img;
    const size_t   n = sizeof(float) * 3 * YOLO_W * YOLO_H;
    uint32_t crc = crc32_sw(p, n);
    uart_puts("[CAP] begin len="); uart_dec((int)n); uart_puts(" crc="); uart_hex32(crc);
    uart_puts(" w="); uart_dec(YOLO_W); uart_puts(" h="); uart_dec(YOLO_H); uart_puts("\n");
    uart_b64(p, n);
    uart_puts("[CAP] end\n");
}
#endif /* CAPTURE_FRAME */

void run_yolo_complete() {
    watchdog_kick();
    unsigned long f = get_timer_freq(); unsigned long t_start = get_timer_count();

    /* B4: frame-skip. With YOLO_INFER_EVERY_N == 1 every frame runs the
     * full graph (current behaviour). With N > 1 only one frame in N
     * runs inference; the other N-1 reuse the cached preds[] but still
     * capture + render (camera DMA + AE keep ticking, HUD/bboxes
     * refresh at the capture cadence). Trades bbox staleness for
     * perceived smoothness. */
#if YOLO_INFER_EVERY_N > 1
    static int skip_counter = 0;
    bool do_inference = ((skip_counter++ % YOLO_INFER_EVERY_N) == 0);
#else
    const bool do_inference = true;
#endif

    uart_puts("[F"); uart_dec(global_frame_counter);
#if YOLO_INFER_EVERY_N > 1
    if (!do_inference) uart_puts("s");
#endif
    uart_puts("] ");
    global_frame_counter++;

    /* Stage timer markers — default to t_start so the [P] deltas are 0
     * on skip frames where the inference block didn't run. */
    unsigned long t_rgb = t_start, t_l0 = t_start;
    unsigned long t_backbone = t_start, t_neck = t_start, t_nms = t_start;

    static bool weights_verified = false;
    if (!weights_verified && WEIGHTS_CRC32 != 0x00000000U) {
        size_t wsz = (size_t)((const uint8_t*)weights_end - (const uint8_t*)weights_start);
        uint32_t actual = crc32_sw((const uint8_t*)weights_start, wsz);
        SAFETY_ASSERT(actual == WEIGHTS_CRC32, "weights CRC mismatch");
        weights_verified = true;
    }

#ifndef SERIAL_BOOT   /* INT8 blobs are not packed into d2m_data.bin (fp32 bench) */
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
#endif /* !SERIAL_BOOT */

    /* Camera capture always runs — keeps the Unicam DMA + AE loop
     * ticking even on frames where we won't run inference. The
     * debayer-to-CHW (which fills cam_frame for YOLO) is inside the
     * inference block since only that path consumes it. The thumbnail
     * in the render block reads unicam_frame_ptr() directly. */
    bool cap_ok = false;
    if (g_use_camera) cap_ok = bsp_frame_acquire();
    /* input_img at function scope — render block also reads it for the
     * test-image fallback path (when g_use_camera == false). */
    const float* input_img = g_use_camera ? cam_frame : test_image;

    if (do_inference) {
        WS_INIT(ws);
        num_preds = 0;

        if (g_use_camera) {
            if (cap_ok) {
                debayer_raw10_to_chw_yolo(cam_frame);
#ifdef CAPTURE_FRAME
                capture_dump_if_due(cam_frame);   /* V185 — once, on frame CAPTURE_FRAME */
#endif
            } else {
                const int n = 3 * YOLO_W * YOLO_H;
                for (int i = 0; i < n; i++) cam_frame[i] = 0.0f;
            }
        }

    int hw = YOLO_W * YOLO_H;
#ifdef USE_INT8_W8A8
    /* W8A8: quantize fp32 input → int8 buf_B using L0's actual
     * scale_in from the bin (auto-tracks any --scale_safety_margin
     * applied in calibrate_int8.py). test_image.bin is in [0,255]
     * (V169 fossil) — scale down before quantizing. */
    float l0_si = w8a8_l0_scale_in();
    float inv_si = 1.0f / l0_si;
    float to_q = (input_img[0] > 1.5f) ? (inv_si / 255.0f) : inv_si;
    int n3 = 3 * hw;
    for (int ii = 0; ii < n3; ii++) {
        float f = input_img[ii] * to_q;
        int32_t q = (int32_t)(f + (f >= 0 ? 0.5f : -0.5f));
        if (q >  127) q =  127;
        if (q < -128) q = -128;
        buf_B[ii] = (int8_t)q;
    }
#else
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
#endif
    t_rgb = get_timer_count();

    /* ── Backbone ─────────────────────────────────────────────────────── */
    LayerHandle L0 = LAYER_LOAD(ws, 16*3*3*3, 16, "L0");
    CONV2D(buf_B, YOLO_H, YOLO_W, 3, L0, 16, 3, 2, 1, true, buf_A);
    LOG_ABSMAX("L0", buf_A, 16 * YOLO_S2H * YOLO_S2W, L0);
    t_l0 = get_timer_count();
    prof_reset();   /* V183 per-layer profile starts after L0 (it has its own bucket) */

    LayerHandle L1 = LAYER_LOAD(ws, 32*16*3*3, 32, "L1");
    CONV2D(buf_A, YOLO_S2H, YOLO_S2W, 16, L1, 32, 3, 2, 1, true, buf_B);
    LOG_ABSMAX("L1", buf_B, 32 * YOLO_S4H * YOLO_S4W, L1);                                     prof_mark("L1  conv3x3 s2 16->32 @S4");
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S4H, YOLO_S4W, 32, 32, 1, true, ws, "L2");  prof_mark("L2  C2f n1  32ch @S4");

    LayerHandle L3 = LAYER_LOAD(ws, 64*32*3*3, 64, "L3");
    CONV2D(buf_A, YOLO_S4H, YOLO_S4W, 32, L3, 64, 3, 2, 1, true, buf_B);                     prof_mark("L3  conv3x3 s2 32->64 @S8");
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S8H, YOLO_S8W, 64, 64, 2, true, ws, "L4");
    COPY_TENSOR(buf_A, save_L4, 64 * YOLO_S8H * YOLO_S8W);                                   prof_mark("L4  C2f n2  64ch @S8");

    LayerHandle L5 = LAYER_LOAD(ws, 128*64*3*3, 128, "L5");
    CONV2D(buf_A, YOLO_S8H, YOLO_S8W, 64, L5, 128, 3, 2, 1, true, buf_B);                    prof_mark("L5  conv3x3 s2 64->128 @S16");
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S16H, YOLO_S16W, 128, 128, 2, true, ws, "L6");
    COPY_TENSOR(buf_A, save_L6, 128 * YOLO_S16H * YOLO_S16W);                                prof_mark("L6  C2f n2  128ch @S16");

    LayerHandle L7 = LAYER_LOAD(ws, 256*128*3*3, 256, "L7");
    CONV2D(buf_A, YOLO_S16H, YOLO_S16W, 128, L7, 256, 3, 2, 1, true, buf_B);
    LOG_ABSMAX("L7", buf_B, 256 * YOLO_S32H * YOLO_S32W, L7);                                prof_mark("L7  conv3x3 s2 128->256 @S32");
    c2f_real_inference(buf_B, buf_A, scratch, YOLO_S32H, YOLO_S32W, 256, 256, 1, true, ws, "L8"); prof_mark("L8  C2f n1  256ch @S32");
    sppf_real_inference(buf_A, buf_B, scratch, YOLO_S32H, YOLO_S32W, 256, ws);
    COPY_TENSOR(buf_B, save_SPPF, 256 * YOLO_S32H * YOLO_S32W);                              prof_mark("SPPF      256ch @S32");

    t_backbone = get_timer_count();

    /* ── Neck (PAN). v8 differs from v5: no extra 1x1 reductions between
     *    upsamples — the C2f modules handle the channel reduction. ───── */

    /* L10 upsample(SPPF) → L11 concat with L6 → L12 C2f → mid-P4 */
    UPSAMPLE2X(buf_B, buf_A, YOLO_S32H, YOLO_S32W, 256);
    CONCAT_TENSOR(buf_A, 256, save_L6, 128, scratch, YOLO_S16H * YOLO_S16W);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S16H, YOLO_S16W, 384, 128, 1, false, ws, "L12");
    COPY_TENSOR(buf_A, save_P4mid, 128 * YOLO_S16H * YOLO_S16W);                             prof_mark("L12 up+cat+C2f 384->128 @S16");

    /* L13 upsample → L14 concat with L4 → L15 C2f → P3 head input */
    UPSAMPLE2X(buf_A, buf_B, YOLO_S16H, YOLO_S16W, 128);
    CONCAT_TENSOR(buf_B, 128, save_L4, 64, scratch, YOLO_S8H * YOLO_S8W);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S8H, YOLO_S8W, 192, 64, 1, false, ws, "L15");
    COPY_TENSOR(buf_A, save_P3, 64 * YOLO_S8H * YOLO_S8W);                                   prof_mark("L15 up+cat+C2f 192->64 @S8");

    /* L16 conv 3x3 s=2 → L17 concat with mid-P4 → L18 C2f → P4 head input */
    LayerHandle L16 = LAYER_LOAD(ws, 64*64*3*3, 64, "L16");
    CONV2D(buf_A, YOLO_S8H, YOLO_S8W, 64, L16, 64, 3, 2, 1, true, buf_B);                    prof_mark("L16 conv3x3 s2 64->64 @S16");
    CONCAT_TENSOR(buf_B, 64, save_P4mid, 128, scratch, YOLO_S16H * YOLO_S16W);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S16H, YOLO_S16W, 192, 128, 1, false, ws, "L18");
    COPY_TENSOR(buf_A, save_P4, 128 * YOLO_S16H * YOLO_S16W);                                prof_mark("L18 cat+C2f 192->128 @S16");

    /* L19 conv 3x3 s=2 → L20 concat with SPPF → L21 C2f → P5 head input */
    LayerHandle L19 = LAYER_LOAD(ws, 128*128*3*3, 128, "L19");
    CONV2D(buf_A, YOLO_S16H, YOLO_S16W, 128, L19, 128, 3, 2, 1, true, buf_B);                prof_mark("L19 conv3x3 s2 128->128 @S32");
    CONCAT_TENSOR(buf_B, 128, save_SPPF, 256, scratch, YOLO_S32H * YOLO_S32W);
    c2f_real_inference(scratch, buf_A, buf_B, YOLO_S32H, YOLO_S32W, 384, 256, 1, false, ws, "L21"); prof_mark("L21 cat+C2f 384->256 @S32");

    t_neck = get_timer_count();

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
    CONV2D (buf_A,    YOLO_S32H, YOLO_S32W, 256, wb_p5_0, 64, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S32H, YOLO_S32W, 64,  wb_p5_1, 64, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S32H, YOLO_S32W, 64,  wb_p5_2, 64, false,       head_box);
    CONV2D (buf_A,    YOLO_S32H, YOLO_S32W, 256, wc_p5_0, 80, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S32H, YOLO_S32W, 80,  wc_p5_1, 80, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S32H, YOLO_S32W, 80,  wc_p5_2, 80, false,       head_cls);
    LOG_ABSMAX("P5_BOX", head_box, 64 * YOLO_S32H * YOLO_S32W, wb_p5_2);
    LOG_ABSMAX("P5_CLS", head_cls, 80 * YOLO_S32H * YOLO_S32W, wc_p5_2);
    DECODE_HEAD(head_box, head_cls, YOLO_S32H, YOLO_S32W, 32, wb_p5_2, wc_p5_2);            prof_mark("P5 head 6 conv + decode @S32");

    /* P4 head — input in save_P4. */
    CONV2D (save_P4,  YOLO_S16H, YOLO_S16W, 128, wb_p4_0, 64, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S16H, YOLO_S16W, 64,  wb_p4_1, 64, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S16H, YOLO_S16W, 64,  wb_p4_2, 64, false,       head_box);
    CONV2D (save_P4,  YOLO_S16H, YOLO_S16W, 128, wc_p4_0, 80, 3, 1, 1, true, head_tmp);
    CONV2D (head_tmp, YOLO_S16H, YOLO_S16W, 80,  wc_p4_1, 80, 3, 1, 1, true, scratch);
    CONV1X1(scratch,  YOLO_S16H, YOLO_S16W, 80,  wc_p4_2, 80, false,       head_cls);
    DECODE_HEAD(head_box, head_cls, YOLO_S16H, YOLO_S16W, 16, wb_p4_2, wc_p4_2);            prof_mark("P4 head 6 conv + decode @S16");

    /* P3 head — input in save_P3. */
#ifdef HEAD_PROF
    #define HP_MARK(s) prof_mark(s)
#else
    #define HP_MARK(s) ((void)0)
#endif
    CONV2D (save_P3,  YOLO_S8H, YOLO_S8W,  64,  wb_p3_0, 64, 3, 1, 1, true, head_tmp);     HP_MARK("  P3.box0 3x3 64->64");
    CONV2D (head_tmp, YOLO_S8H, YOLO_S8W,  64,  wb_p3_1, 64, 3, 1, 1, true, scratch);      HP_MARK("  P3.box1 3x3 64->64");
    CONV1X1(scratch,  YOLO_S8H, YOLO_S8W,  64,  wb_p3_2, 64, false,       head_box);       HP_MARK("  P3.box2 1x1 64->64");
    CONV2D (save_P3,  YOLO_S8H, YOLO_S8W,  64,  wc_p3_0, 80, 3, 1, 1, true, head_tmp);     HP_MARK("  P3.cls0 3x3 64->80");
    CONV2D (head_tmp, YOLO_S8H, YOLO_S8W,  80,  wc_p3_1, 80, 3, 1, 1, true, scratch);      HP_MARK("  P3.cls1 3x3 80->80");
    CONV1X1(scratch,  YOLO_S8H, YOLO_S8W,  80,  wc_p3_2, 80, false,       head_cls);       HP_MARK("  P3.cls2 1x1 80->80");
    LOG_ABSMAX("P3_BOX", head_box, 64 * YOLO_S8H * YOLO_S8W, wb_p3_2);
    LOG_ABSMAX("P3_CLS", head_cls, 80 * YOLO_S8H * YOLO_S8W, wc_p3_2);
    DECODE_HEAD(head_box, head_cls, YOLO_S8H, YOLO_S8W, 8, wb_p3_2, wc_p3_2);               prof_mark("P3 head 6 conv + decode @S8");
    #undef HP_MARK

    t_nms = get_timer_count();

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
    update_tracker();
    }  /* end if (do_inference) — render block below reuses preds[]/tracks[] */

    /* Canvas layout:
     *   y=0..59   HUD top bar
     *   y=60..419 canvas + bboxes in the CAM_DISP_* area (+ DEBUG thumbnail)
     *   y=420..479 HUD bottom bar (detection cards)
     *
     * SHOW_CAMERA=1 (V186, default): the canvas IS the live frame —
     * camera_render_fullres() debayers the 1536×864 RAW10 into 640×360
     * letterboxed at rows 60..419 (the geometry CAM_DISP_* was derived
     * from), so the boxes land on the objects. ~20 ms single-core.
     * SHOW_CAMERA=0: the V167 dark canvas (saves those ms). */
    extern void debayer_raw10_to_thumbnail(const uint8_t* raw, uint8_t* fb,
                                           uint32_t pitch, int x_off, int y_off,
                                           int thumb_w, int thumb_h);

    if (g_use_camera) {
#if SHOW_CAMERA
        camera_render_fullres((uint8_t*)lfb, pitch);
#else
        /* Clear the canvas region (rows 60..419) to solid black. */
        for (int y = 60; y < 420; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)lfb + (uint32_t)y * pitch);
            for (int x = 0; x < 640; x++) row[x] = 0xFF000000u;
        }
#endif
#if DEBUG
        /* Camera thumbnail in the top-right corner — debug only. */
        const int THUMB_W = 128, THUMB_H = 72;
        const int THUMB_X = 640 - THUMB_W - 8;   /* 504 */
        const int THUMB_Y = 64;
        debayer_raw10_to_thumbnail(unicam_frame_ptr(), (uint8_t*)lfb, pitch,
                                   THUMB_X, THUMB_Y, THUMB_W, THUMB_H);
        draw_rect(THUMB_X - 1, THUMB_Y - 1, THUMB_W + 2, THUMB_H + 2,
                  0xFF00C0FFu, 1);
#endif
    } else {
        draw_fill(0xFF222222);
        draw_tensor_image_fullscreen(input_img);
    }

    /* Bbox scale: box coords are in YOLO_W×YOLO_H space, content letterboxed
     * to rows [YOLO_LETTERBOX_TOP, +YOLO_CONTENT_H). V192:
     *  - camera: camera_render_fullres() paints the full sensor to 640×360 at
     *    FB y-offset 60 (camera_debayer DISP_*). Model 320×180 content ↔ that
     *    640×360 → ×2 both axes; y-origin 60 − TOP·2.
     *  - test image: draw_tensor_image_fullscreen stretches the whole tensor
     *    (incl. the black bars) to 640×480. */
    const float disp_scale  = g_use_camera ? (640.0f / (float)YOLO_W)
                                           : (640.0f / (float)YOLO_W);
    const float disp_scaleY = g_use_camera ? (360.0f / (float)YOLO_CONTENT_H)
                                           : (480.0f / (float)YOLO_H);
    const int   disp_xoff   = 0;
    const int   disp_yoff   = g_use_camera ? (60 - (int)(YOLO_LETTERBOX_TOP * (360.0f / YOLO_CONTENT_H))) : 0;
    const int   disp_right  = disp_xoff + 640;
    const int   disp_bottom = disp_yoff + (g_use_camera ? (60 + 360) : 480);

    int valid_boxes = 0;
    static const uint32_t bbox_palette[6] = {
        0xFF00C0FFu, 0xFF00FF80u, 0xFFFF00FFu,
        0xFFFFC000u, 0xFF80FF00u, 0xFFFF4080u
    };

    for (int ti = 0; ti < MAX_TRACKS; ti++) {
        if (tracks[ti].id == 0) continue;
        const Box& b = tracks[ti].box;
        if (b.conf <= CONF_THRESH) continue;

        valid_boxes++;
        uart_puts("[DET] id="); uart_dec(tracks[ti].id);
        uart_puts(" c=");       uart_dec(b.cls);
        uart_puts(" %=");       uart_dec((int)(b.conf * 100));
        if (tracks[ti].frames_missed > 0) {
            uart_puts(" miss="); uart_dec(tracks[ti].frames_missed);
        }
        uart_puts("\n");

        int box_w = (int)(b.w * disp_scale);
        int box_h = (int)(b.h * disp_scaleY);
        int cx    = (int)(b.x * disp_scale)  + disp_xoff;
        int cy    = (int)(b.y * disp_scaleY) + disp_yoff;

        int left = cx - (box_w / 2);
        int top  = cy - (box_h / 2);

        if (left < disp_xoff) { left = disp_xoff; }
        if (top  < disp_yoff) { top  = disp_yoff; }
        if (left + box_w > disp_right)  { box_w = disp_right  - left; }
        if (top  + box_h > disp_bottom) { box_h = disp_bottom - top; }

        if (box_w > 2 && box_h > 2) {
            /* Color by track ID so two same-class detections are visually
             * distinguishable; cycles every 6 IDs. */
            uint32_t color = bbox_palette[((unsigned)tracks[ti].id) % 6];
            draw_rect(left, top, box_w, box_h, color, 3);

            /* Label format: "person #3 67%" */
            const char* nm = (b.cls >= 0 && b.cls < 80) ? coco_names[b.cls] : "?";
            int pct = (int)(b.conf * 100.0f);
            if (pct > 99) pct = 99;
            int id  = tracks[ti].id;
            char lbl[48]; int ln = 0;
            while (*nm && ln < 30) lbl[ln++] = *nm++;
            lbl[ln++] = ' '; lbl[ln++] = '#';
            /* Up to 4 digits for the track ID. */
            if (id >= 1000) { lbl[ln++] = '0' + (id / 1000) % 10; }
            if (id >= 100)  { lbl[ln++] = '0' + (id / 100)  % 10; }
            if (id >= 10)   { lbl[ln++] = '0' + (id / 10)   % 10; }
            lbl[ln++] = '0' + (id % 10);
            lbl[ln++] = ' ';
            if (pct >= 10) lbl[ln++] = '0' + (pct / 10);
            lbl[ln++] = '0' + (pct % 10);
            lbl[ln++] = '%';
            lbl[ln] = '\0';
            int label_y = (top - 18 >= disp_yoff) ? top - 18 : top + 4;
            draw_text(left + 2, label_y, lbl, color, 0xFF000000u, 2);
        }
    }
    if (valid_boxes == 0) uart_puts("[DET] none\n");
    heartbeat_counter++;

    unsigned long t_end = get_timer_count();

    /* B0: per-stage profiling. cap → rgb prep, l0 → first conv2d,
     * backbone → through L8, neck → through L23, head → up to NMS,
     * total → end. Suppressed on skip frames since the model stages
     * didn't run; t_rgb..t_nms would all equal t_start. */
    if (do_inference) {
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
        /* HUD cards come from the same tracks the bboxes use — pick the top
         * 3 active tracks by confidence so cards and bboxes always agree. */
        HudPred hud_preds[3]; int hud_n = 0;
        float   hud_conf[3];
        for (int ti = 0; ti < MAX_TRACKS; ti++) {
            if (tracks[ti].id == 0) continue;
            const Box& b = tracks[ti].box;
            if (b.conf <= CONF_THRESH) continue;
            int slot = hud_n;
            if (hud_n < 3) hud_n++;
            else if (b.conf > hud_conf[2]) slot = 2;
            else continue;
            /* Insertion sort by descending conf (3 slots — trivial). */
            while (slot > 0 && b.conf > hud_conf[slot-1]) {
                hud_conf[slot]  = hud_conf[slot-1];
                hud_preds[slot] = hud_preds[slot-1];
                slot--;
            }
            hud_conf[slot]       = b.conf;
            hud_preds[slot].cls  = b.cls;
            hud_preds[slot].conf = b.conf;
        }
        hud_update_state(hud_preds, hud_n, global_frame_counter,
                         (int)duration_ms, (int)imx708_ae_cit_get());
        hud_render((uint8_t*)lfb, pitch);
    }

    /* B4: preds[] is NOT cleared here — skip-frame renders reuse it
     * until the next inference frame overwrites the array. */
    video_flush();

    uart_puts("[T] "); uart_dec((t_end-t_start)*1000/f); uart_puts("ms\n");

    /* V183: per-layer profile AFTER [P]/[T] so the marks never inflate them.
     * Only meaningful on real HW; hwbench.py parses the "  name: N us" lines. */
    if (do_inference) prof_dump();
}
