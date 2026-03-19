/* File: src/camera_debayer.cpp
 * V111+BGGR — RAW10-packed BGGR 1536×864 → float32 CHW 320×320 + framebuffer display.
 *
 * BGGR fix: IMX708 reports SBGGR10_1X10/RAW (libcamera confirmed).
 *   BGGR layout:  (even row, even col) = B
 *                 (even row, odd  col) = Gb
 *                 (odd  row, even col) = Gr
 *                 (odd  row, odd  col) = R
 *
 * CSI-2 RAW10 packed format: every 5 bytes hold 4 pixels (10-bit each).
 *   Byte layout per group:
 *     byte[0] = P0 bits[9:2]  (MSB 8 bits)
 *     byte[1] = P1 bits[9:2]
 *     byte[2] = P2 bits[9:2]
 *     byte[3] = P3 bits[9:2]
 *     byte[4] = P3[1:0]<<6 | P2[1:0]<<4 | P1[1:0]<<2 | P0[1:0]<<0
 *
 * For 8-bit quality output (YOLO input / display), we only use the MSB 8 bits
 * (byte[0..3]), ignoring byte[4]. This is equivalent to >> 2 on the 10-bit value.
 * This simplifies the debayer to a stride-aware byte access pattern.
 *
 * Pipeline:
 *   1. Center-crop: x_start=336 pixels (even), crop region = 864×864 pixels
 *   2. Nearest-neighbor scale 864→320 using Q8 fixed-point step=691
 *   3. Simple bilinear RGGB demosaic: G = (Gr + Gb) / 2
 *   4. Normalise: divide by 255.0  (output range [0, 1])
 *   5. Pack into CHW: [R plane][G plane][B plane]
 *
 * RAW10 pixel access: pixel column `col` within a line of `stride` bytes:
 *   group = col / 4;  pos_in_group = col % 4;
 *   msb8 = row[group * 5 + pos_in_group];
 */
#include <stdint.h>
#include <arm_neon.h>

// Provided by camera_unicam.cpp
extern const uint8_t* unicam_frame_ptr();
extern int            unicam_frame_w();   // byte stride (1920)
extern int            unicam_frame_h();   // 864
extern int            unicam_pixel_w();   // 1536

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

/* Read MSB 8 bits of a RAW10-packed pixel at column `col` in a row.
 * row points to the start of the line (stride = unicam_frame_w() bytes).
 * This extracts byte[group*5 + pos], which holds bits[9:2] of the 10-bit pixel. */
static inline uint8_t raw10_msb8(const uint8_t* row, int col) {
    int group = col >> 2;          /* col / 4 */
    int pos   = col & 3;           /* col % 4 */
    return row[group * 5 + pos];
}

// ---- Conversion of 8 gathered pixels using NEON ----
static inline void store8_to_chw(const uint8_t* r8, const uint8_t* g8, const uint8_t* b8,
                                  float* r_ptr, float* g_ptr, float* b_ptr) {
    const float32x4_t vscale = vdupq_n_f32(1.0f / 255.0f);

    uint8x8_t vr = vld1_u8(r8);
    uint8x8_t vg = vld1_u8(g8);
    uint8x8_t vb = vld1_u8(b8);

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

void debayer_raw10_to_chw320(float* dst) {
    const uint8_t* frame = unicam_frame_ptr();
    const int byte_stride = unicam_frame_w();   // = 1920 bytes per line

    float* r_plane = dst;
    float* g_plane = dst + OUT_H * OUT_W;
    float* b_plane = dst + 2 * OUT_H * OUT_W;

    for (int oy = 0; oy < OUT_H; oy++) {
        // Map output row → source row, snap to even (RGGB boundary)
        int src_y = ((oy * STEP_Q8) >> 8);
        src_y &= ~1;   // round down to even
        if (src_y + 1 >= CROP_SZ) src_y = CROP_SZ - 2;

        const uint8_t* row0 = frame + src_y       * byte_stride;
        const uint8_t* row1 = frame + (src_y + 1) * byte_stride;

        float* r_row = r_plane + oy * OUT_W;
        float* g_row = g_plane + oy * OUT_W;
        float* b_row = b_plane + oy * OUT_W;

        // Process 8 output columns per NEON iteration
        int ox = 0;
        for (; ox <= OUT_W - 8; ox += 8) {
            uint8_t r8[8], g8[8], b8[8];
            for (int k = 0; k < 8; k++) {
                int src_x = (((ox + k) * STEP_Q8) >> 8) + CROP_X;
                src_x &= ~1;   // snap to even BGGR boundary

                // BGGR 2×2 block:  B  Gb   (row0: src_x, src_x+1)
                //                  Gr  R   (row1: src_x, src_x+1)
                uint8_t B  = raw10_msb8(row0, src_x);
                uint8_t Gb = raw10_msb8(row0, src_x + 1);
                uint8_t Gr = raw10_msb8(row1, src_x);
                uint8_t R  = raw10_msb8(row1, src_x + 1);

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

            uint8_t B  = raw10_msb8(row0, src_x);
            uint8_t Gb = raw10_msb8(row0, src_x + 1);
            uint8_t Gr = raw10_msb8(row1, src_x);
            uint8_t R  = raw10_msb8(row1, src_x + 1);

            r_row[ox] = R  * (1.0f / 255.0f);
            g_row[ox] = (uint8_t)(((unsigned)Gr + Gb) >> 1) * (1.0f / 255.0f);
            b_row[ox] = B  * (1.0f / 255.0f);
        }
    }
}

// Render the last captured RAW10 frame directly to the framebuffer at maximum resolution.
// Writes 480×480 pixels (center-crop of 864×864) to x=[80,560] in a 640×480 framebuffer.
void debayer_raw10_to_fb(uint8_t* fb, uint32_t pitch) {
    const uint8_t* frame = unicam_frame_ptr();
    const int byte_stride = unicam_frame_w();   // = 1920 bytes per line

    for (int oy = 0; oy < DISP_H; oy++) {
        int src_y = (oy * STEP_Q8_FB) >> 8;
        src_y &= ~1;
        if (src_y + 1 >= CROP_SZ) src_y = CROP_SZ - 2;

        const uint8_t* row0 = frame + src_y       * byte_stride;
        const uint8_t* row1 = frame + (src_y + 1) * byte_stride;

        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)oy * pitch);

        // Left letterbox (black)
        for (int x = 0; x < DISP_XOFF; x++) fb_row[x] = 0xFF000000u;

        // Debayer + scale → direct ARGB write to framebuffer
        for (int ox = 0; ox < DISP_W; ox++) {
            int src_x = ((ox * STEP_Q8_FB) >> 8) + CROP_X;
            src_x &= ~1;

            // BGGR: (even,even)=B  (even,odd)=Gb  (odd,even)=Gr  (odd,odd)=R
            uint8_t B  = raw10_msb8(row0, src_x);
            uint8_t Gb = raw10_msb8(row0, src_x + 1);
            uint8_t Gr = raw10_msb8(row1, src_x);
            uint8_t R  = raw10_msb8(row1, src_x + 1);
            uint8_t G  = (uint8_t)(((unsigned)Gr + Gb) >> 1);

            // Brightness stretch: subtract black level (16) and apply 4x gain, clamp to 255
            int rb = ((int)R - 16) * 4; if (rb < 0) rb = 0; if (rb > 255) rb = 255;
            int gb = ((int)G - 16) * 4; if (gb < 0) gb = 0; if (gb > 255) gb = 255;
            int bb = ((int)B - 16) * 4; if (bb < 0) bb = 0; if (bb > 255) bb = 255;

            fb_row[DISP_XOFF + ox] = 0xFF000000u | ((uint32_t)bb << 16) | ((uint32_t)gb << 8) | (uint32_t)rb;
        }

        // Right letterbox (black)
        for (int x = DISP_XOFF + DISP_W; x < FB_W; x++) fb_row[x] = 0xFF000000u;
    }
}
