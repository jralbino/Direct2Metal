/* File: src/conv2d.cpp
 *
 * conv2d_neon_3x3() — the function called by kernel.cpp's inference loop.
 *
 * Dispatch:
 *   - If C_out is divisible by 4: use conv2d_neon_4ch() which processes
 *     4 output channels per NEON vector using repacked weights.
 *   - Otherwise: fall back to scalar conv2d_cpp() for correctness.
 *
 * Weight layout expected: NEON repacked [C_out/4][C_in*K*K][4]
 * as produced by tools/export_model.py::repack_for_neon().
 */

#include "ops.h"

void conv2d_neon_3x3(const float* in, int H_in, int W_in, int C_in,
                     const float* w,  int C_out,
                     float* out, int stride, int pad) {
    if (C_out % 4 == 0) {
        /* Fast path: NEON 4-channel kernel with repacked weights */
        conv2d_neon_4ch(in, H_in, W_in, C_in,
                        w,  C_out, 3, stride, pad, out);
    } else {
        /* Fallback: generic scalar conv2d (e.g. C_out not multiple of 4) */
        conv2d_cpp(in, H_in, W_in, C_in,
                   w,  C_out, 3, stride, pad, out);
    }
}
