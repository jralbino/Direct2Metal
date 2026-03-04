/* File: src/camera_debayer.cpp
 * RAW8 RGGB 1536×864 → float32 CHW 320×320 conversion.
 *
 * Pipeline:
 *   1. Center-crop: x_start=336 (even), crop region = 864×864
 *   2. Nearest-neighbor scale 864→320 using Q8 fixed-point step=691
 *   3. Simple bilinear RGGB demosaic: G = (Gr + Gb) / 2
 *   4. Normalise: divide by 255.0  (output range [0, 1])
 *   5. Pack into CHW: [R plane][G plane][B plane]
 *
 * NEON: the scalar gather (non-contiguous source pixels) is unavoidable for
 * nearest-neighbour; NEON accelerates the uint8→float32 conversion for 8
 * output pixels at a time (3× batched stores into R/G/B planes).
 */
#include <stdint.h>
#include <arm_neon.h>

// Provided by camera_unicam.cpp
extern const uint8_t* unicam_frame_ptr();
extern int            unicam_frame_w();
extern int            unicam_frame_h();

// Output image dimensions (YOLO inference)
#define OUT_W    320
#define OUT_H    320

// Source crop parameters (RGGB-aligned, even boundaries)
// Center-crop 864×864 from 1536×864:  x_start = (1536-864)/2 = 336 (even ✓)
#define CROP_X   336
#define CROP_SZ  864   // square crop side in source pixels

// Q8 fixed-point step: 864 * 256 / 320 = 691.2 → 691
#define STEP_Q8  691

// Framebuffer display params: 480×480 letterboxed in 640×480
// Q8 step: 864 * 256 / 480 = 460.8 → 461
#define FB_W        640
#define FB_H        480
#define DISP_W      480
#define DISP_H      480
#define DISP_XOFF    80
#define STEP_Q8_FB  461

// ---- Conversion of 8 gathered pixels using NEON ----
// Converts uint8[8] arrays for R, G, B into float32 and stores into CHW planes.
static inline void store8_to_chw(const uint8_t* r8, const uint8_t* g8, const uint8_t* b8,
                                  float* r_ptr, float* g_ptr, float* b_ptr) {
    const float32x4_t vscale = vdupq_n_f32(1.0f / 255.0f);

    uint8x8_t vr = vld1_u8(r8);
    uint8x8_t vg = vld1_u8(g8);
    uint8x8_t vb = vld1_u8(b8);

    // Widen u8 → u16 → u32 → f32 for first 4 elements
    uint16x8_t vr16 = vmovl_u8(vr);
    uint16x8_t vg16 = vmovl_u8(vg);
    uint16x8_t vb16 = vmovl_u8(vb);

    float32x4_t rf0 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(vr16)));
    float32x4_t rf1 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(vr16)));
    float32x4_t gf0 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(vg16)));
    float32x4_t gf1 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(vg16)));
    float32x4_t bf0 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(vb16)));
    float32x4_t bf1 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(vb16)));

    vst1q_f32(r_ptr,     vmulq_f32(rf0, vscale));
    vst1q_f32(r_ptr + 4, vmulq_f32(rf1, vscale));
    vst1q_f32(g_ptr,     vmulq_f32(gf0, vscale));
    vst1q_f32(g_ptr + 4, vmulq_f32(gf1, vscale));
    vst1q_f32(b_ptr,     vmulq_f32(bf0, vscale));
    vst1q_f32(b_ptr + 4, vmulq_f32(bf1, vscale));
}

void debayer_raw8_to_chw320(float* dst) {
    const uint8_t* frame = unicam_frame_ptr();
    const int src_stride = unicam_frame_w();   // = 1536

    float* r_plane = dst;
    float* g_plane = dst + OUT_H * OUT_W;
    float* b_plane = dst + 2 * OUT_H * OUT_W;

    for (int oy = 0; oy < OUT_H; oy++) {
        // Map output row → source row, snap to even (RGGB boundary)
        int src_y = ((oy * STEP_Q8) >> 8);
        src_y &= ~1;   // round down to even
        if (src_y + 1 >= CROP_SZ) src_y = CROP_SZ - 2;  // clamp

        const uint8_t* row0 = frame + src_y       * src_stride;
        const uint8_t* row1 = frame + (src_y + 1) * src_stride;

        float* r_row = r_plane + oy * OUT_W;
        float* g_row = g_plane + oy * OUT_W;
        float* b_row = b_plane + oy * OUT_W;

        // Process 8 output columns per NEON iteration
        int ox = 0;
        for (; ox <= OUT_W - 8; ox += 8) {
            uint8_t r8[8], g8[8], b8[8];
            for (int k = 0; k < 8; k++) {
                int src_x = ((( ox + k) * STEP_Q8) >> 8) + CROP_X;
                src_x &= ~1;                             // snap to even
                if (src_x + 1 >= src_stride) src_x = src_stride - 2; // clamp

                // RGGB 2×2 block:  R  Gr
                //                  Gb  B
                uint8_t R  = row0[src_x];
                uint8_t Gr = row0[src_x + 1];
                uint8_t Gb = row1[src_x];
                uint8_t B  = row1[src_x + 1];

                r8[k] = R;
                g8[k] = (uint8_t)(((unsigned)Gr + Gb) >> 1);
                b8[k] = B;
            }
            store8_to_chw(r8, g8, b8, r_row + ox, g_row + ox, b_row + ox);
        }

        // Scalar tail (up to 7 remaining columns)
        for (; ox < OUT_W; ox++) {
            int src_x = ((ox * STEP_Q8) >> 8) + CROP_X;
            src_x &= ~1;
            if (src_x + 1 >= src_stride) src_x = src_stride - 2;

            uint8_t R  = row0[src_x];
            uint8_t Gr = row0[src_x + 1];
            uint8_t Gb = row1[src_x];
            uint8_t B  = row1[src_x + 1];

            r_row[ox] = R  * (1.0f / 255.0f);
            g_row[ox] = (uint8_t)(((unsigned)Gr + Gb) >> 1) * (1.0f / 255.0f);
            b_row[ox] = B  * (1.0f / 255.0f);
        }
    }
}

// Render the last captured RAW8 frame directly to the framebuffer at maximum resolution.
// Writes 480×480 pixels (center-crop of 864×864) to x=[80,560] in a 640×480 framebuffer.
// Left/right 80-pixel letterbox columns are filled with black.
// Pixel format: 0xFF_BB_GG_RR (matches draw_pixel convention in video.cpp).
void debayer_raw8_to_fb(uint8_t* fb, uint32_t pitch) {
    const uint8_t* frame = unicam_frame_ptr();
    const int src_stride = unicam_frame_w();   // = 1536

    for (int oy = 0; oy < DISP_H; oy++) {
        // Map output row → source row, snap to even RGGB boundary
        int src_y = (oy * STEP_Q8_FB) >> 8;
        src_y &= ~1;
        if (src_y + 1 >= CROP_SZ) src_y = CROP_SZ - 2;

        const uint8_t* row0 = frame + src_y       * src_stride;
        const uint8_t* row1 = frame + (src_y + 1) * src_stride;

        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)oy * pitch);

        // Left letterbox (black)
        for (int x = 0; x < DISP_XOFF; x++) fb_row[x] = 0xFF000000u;

        // Debayer + scale → direct ARGB write to framebuffer
        for (int ox = 0; ox < DISP_W; ox++) {
            int src_x = ((ox * STEP_Q8_FB) >> 8) + CROP_X;
            src_x &= ~1;
            if (src_x + 1 >= src_stride) src_x = src_stride - 2;

            // RGGB 2×2 block
            uint8_t R  = row0[src_x];
            uint8_t Gr = row0[src_x + 1];
            uint8_t Gb = row1[src_x];
            uint8_t B  = row1[src_x + 1];
            uint8_t G  = (uint8_t)(((unsigned)Gr + Gb) >> 1);

            fb_row[DISP_XOFF + ox] = 0xFF000000u | ((uint32_t)B << 16) | ((uint32_t)G << 8) | R;
        }

        // Right letterbox (black)
        for (int x = DISP_XOFF + DISP_W; x < FB_W; x++) fb_row[x] = 0xFF000000u;
    }
}
