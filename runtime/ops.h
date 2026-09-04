/* File: runtime/ops.h — Bare-metal neural network operator library.
 *
 * All tensors use CHW (channel-first) layout:
 * buf[channel][row][col] = buf[c*H*W + y*W + x]
 *
 * No standard library is required; this is safe for -ffreestanding -nostdlib.
 *
 * The K>1 FP32 convolutions live in bsp/multicore.cpp (conv2d_partial /
 * conv2d_partial_8ch) and are reached through parallel_conv2d(); this header
 * exports the FP32 1×1 kernel, the pooling / resampling helpers, and the
 * G2 Tier 2 INT8 kernels (runtime/ops_int8.cpp, built behind USE_INT8*).
 *
 * V183: pruned the declarations that had no definition anywhere
 * (batchnorm_inplace, leaky_relu_inplace, maxpool2x2, conv2d_cpp*,
 * conv2d_neon_4ch*, conv1x1, yolo_decode/BBox, silu_inplace, c3_block,
 * camera_to_tensor_320) together with their dead bodies in ops.cpp.
 */
#ifndef OPS_H
#define OPS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * FP32 Conv 1×1 with bias + optional fused SiLU — the per-core slice kernel.
 * Computes output channels [C_out_start, C_out_end) of a C_out_total layer.
 * p_step-outer tiling (a C_in × 32-pixel input tile is loaded once and reused
 * across all output-channel groups) + B2 8-output-channel hot path + PRFM.
 * in  : [C_in][H][W]     w : [C_out_total][C_in]     b : [C_out_total] or NULL
 * out : [C_out_total][H][W]
 * ------------------------------------------------------------------ */
void ops_neon_conv1x1_kernel(const float* in, int H, int W, int C_in,
                             const float* w, const float* b,
                             int C_out_start, int C_out_end, int C_out_total,
                             bool do_silu, float* out);

/* ------------------------------------------------------------------
 * G2 Tier 2 — W8A8 INT8 kernels (runtime/ops_int8.cpp)
 *
 * All inputs / outputs / weights are int8. Bias is int32 (pre-multiplied
 * by 1/(scale_in*scale_w) at calibration time). Out-stage:
 *   acc_int32 * (scale_w * scale_in) → fp32 → optional SiLU → /scale_out
 *   → saturate to int8.
 *
 * Weight layout matches tools/calibrate_int8.py. Tier 2 was closed
 * 2026-05-26 (GOALS.md): on A53 without SDOT it does not beat FP32. Kept
 * behind USE_INT8_W8A8 for a future Pi 4/5 port.
 * ------------------------------------------------------------------ */

/* 1x1 conv: w_flat[C_out * C_in]. */
void conv1x1_int8(const int8_t* in, int H, int W, int C_in,
                  const int8_t* w, const int32_t* bias,
                  float scale_w, float scale_in, float scale_out,
                  int C_out, bool do_silu,
                  int8_t* out);

/* K×K conv, 4 output channels per group (weight layout
 * int8[C_out/4][C_in*K*K][4]). */
void conv2d_neon_4ch_int8(const int8_t* in, int H_in, int W_in, int C_in,
                          const int8_t* w_rep, int C_out, int K, int stride, int pad,
                          float scale_w, float scale_in, float scale_out,
                          const int32_t* bias, bool do_silu,
                          int8_t* out);

/* K×K conv, 8 output channels per group (weight layout
 * int8[C_out/8][C_in*K*K][8]). */
void conv2d_neon_8ch_int8(const int8_t* in, int H_in, int W_in, int C_in,
                          const int8_t* w_rep, int C_out, int K, int stride, int pad,
                          float scale_w, float scale_in, float scale_out,
                          const int32_t* bias, bool do_silu,
                          int8_t* out);

#ifdef __cplusplus
}
#endif

/* Nearest-neighbour 2× upsample (PAN neck).  in [C][H][W] → out [C][2H][2W] */
void upsample2x_nearest(const float* in, float* out, int H, int W, int C);

/* MaxPool 5×5, stride 1, padding 2 (SPPF block). NEON on interior pixels. */
void maxpool5x5_s1_p2(const float* in, float* out, int H, int W, int C);

#endif /* OPS_H */
