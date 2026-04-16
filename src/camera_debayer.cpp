/* File: src/camera_debayer.cpp
 * V124 — RAW10-packed RGGB 1536×864 debayer.
 *   - YOLO path: 864×864 center crop → 320×320 CHW float32.
 *   - Framebuffer path: FULL 1536×864 sensor frame → 640×360 at native 16:9,
 *     centered vertically in 640×480 (60-row top/bottom letterbox).
 *   - V124 Bayer pattern correction: switched back from BGGR → RGGB.
 *     V116 adopted BGGR based on libcamera's "SBGGR10_1X10" tag, but HDMI
 *     was intermittent so the swap was never visually verified. With HDMI
 *     stable in V123 the user observed BLUE skin → R/B channels were swapped.
 *     At this IMX708 configuration (no orientation register write; VPU
 *     firmware + dtoverlay=imx708 sequence) the pixel at (0,0) is RED, so
 *     the effective Bayer pattern is RGGB:
 *         (even,even)=R  (even,odd)=Gr  (odd,even)=Gb  (odd,odd)=B
 *   - Integer-block sampling retained from V123 (no `& ~1` aliasing).
 *   - V122 UART diagnostics (ASCII art / PPM dump / pixel-sample / safe dump)
 *     removed: HDMI is now stable so headless UART debugging is no longer needed.
 */
#include <stdint.h>
#include <arm_neon.h>

extern void uart_puts(const char* s);
extern void uart_dec(int n);
extern void uart_hex(uint32_t d);
extern void watchdog_kick();

extern const uint8_t* unicam_frame_ptr();
extern int            unicam_frame_w();   // byte stride (1920)
extern int            unicam_frame_h();   // 864
extern int            unicam_pixel_w();   // 1536

/* ── Sensor geometry (IMX708 2×2-binned mode) ─────────────────────────────── */
#define SENSOR_PX_W   1536
#define SENSOR_PX_H    864
#define SENSOR_BLK_W  (SENSOR_PX_W / 2)   /* 768 Bayer blocks horizontally   */
#define SENSOR_BLK_H  (SENSOR_PX_H / 2)   /* 432 Bayer blocks vertically     */

/* ── YOLO path: 864×864 center crop → 320×320 ─────────────────────────────── */
#define OUT_W    320
#define OUT_H    320
#define CROP_X   336                      /* (1536 - 864) / 2                */
#define CROP_SZ  864
#define STEP_Q8  691                      /* 864/320 ≈ 2.7 in Q8             */

/* ── Framebuffer path: full 1536×864 → 640×360 letterboxed on 640×480 ──── */
#define FB_W        640
#define FB_H        480
#define DISP_W      640
#define DISP_H      360
#define DISP_XOFF     0
#define DISP_YOFF    60                   /* (480 - 360) / 2                 */

static inline uint8_t raw10_msb8(const uint8_t* row, int col) {
    int group = col >> 2;
    int pos   = col & 3;
    return row[group * 5 + pos];
}

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

/* ── YOLO input: debayer 864×864 center crop → 320×320 CHW float32 ─────── */
void debayer_raw10_to_chw320(float* dst) {
    const uint8_t* frame = unicam_frame_ptr();
    const int byte_stride = unicam_frame_w();

    float* r_plane = dst;
    float* g_plane = dst + OUT_H * OUT_W;
    float* b_plane = dst + 2 * OUT_H * OUT_W;

    for (int oy = 0; oy < OUT_H; oy++) {
        int src_y = ((oy * STEP_Q8) >> 8);
        src_y &= ~1;
        if (src_y + 1 >= CROP_SZ) src_y = CROP_SZ - 2;

        const uint8_t* row0 = frame + src_y       * byte_stride;
        const uint8_t* row1 = frame + (src_y + 1) * byte_stride;

        float* r_row = r_plane + oy * OUT_W;
        float* g_row = g_plane + oy * OUT_W;
        float* b_row = b_plane + oy * OUT_W;

        int ox = 0;
        for (; ox <= OUT_W - 8; ox += 8) {
            uint8_t r8[8], g8[8], b8[8];
            for (int k = 0; k < 8; k++) {
                int src_x = (((ox + k) * STEP_Q8) >> 8) + CROP_X;
                src_x &= ~1;

                /* RGGB: (even,even)=R  (even,odd)=Gr  (odd,even)=Gb  (odd,odd)=B */
                uint8_t R  = raw10_msb8(row0, src_x);
                uint8_t Gr = raw10_msb8(row0, src_x + 1);
                uint8_t Gb = raw10_msb8(row1, src_x);
                uint8_t B  = raw10_msb8(row1, src_x + 1);

                r8[k] = R;
                g8[k] = (uint8_t)(((unsigned)Gr + Gb) >> 1);
                b8[k] = B;
            }
            store8_to_chw(r8, g8, b8, r_row + ox, g_row + ox, b_row + ox);
        }

        for (; ox < OUT_W; ox++) {
            int src_x = ((ox * STEP_Q8) >> 8) + CROP_X;
            src_x &= ~1;

            /* RGGB */
            uint8_t R  = raw10_msb8(row0, src_x);
            uint8_t Gr = raw10_msb8(row0, src_x + 1);
            uint8_t Gb = raw10_msb8(row1, src_x);
            uint8_t B  = raw10_msb8(row1, src_x + 1);

            r_row[ox] = R  * (1.0f / 255.0f);
            g_row[ox] = (uint8_t)(((unsigned)Gr + Gb) >> 1) * (1.0f / 255.0f);
            b_row[ox] = B  * (1.0f / 255.0f);
        }
    }
}

/* ── Test pattern: color bars (unchanged diagnostic) ──────────────────────── */
void debayer_test_pattern_fb(uint8_t* fb, uint32_t pitch) {
    for (int oy = 0; oy < FB_H; oy++) {
        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)oy * pitch);
        uint32_t color;
        int band = (oy / 10) % 4;
        if (band == 0)      color = 0xFF0000FFu;
        else if (band == 1) color = 0xFF00FF00u;
        else if (band == 2) color = 0xFFFF0000u;
        else                color = 0xFFFFFFFFu;
        for (int x = 0; x < (int)FB_W; x++) fb_row[x] = color;
    }
}

/* ── Framebuffer output: full sensor (1536×864) → 640×360 16:9 ─────────── */
void debayer_raw10_to_fb(uint8_t* fb, uint32_t pitch) {
    const uint8_t* frame = unicam_frame_ptr();
    const int byte_stride = unicam_frame_w();  /* 1920 (RAW10-packed stride) */

    /* Top letterbox (black) */
    for (int y = 0; y < DISP_YOFF; y++) {
        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)y * pitch);
        for (int x = 0; x < FB_W; x++) fb_row[x] = 0xFF000000u;
    }

    for (int oy = 0; oy < DISP_H; oy++) {
        /* Integer block mapping: monotonically advancing 2×2 Bayer blocks.
         * Avoids the `& ~1` aliasing that produced duplicate-block artefacts
         * on non-integer scale factors. */
        int blk_y = (oy * SENSOR_BLK_H) / DISP_H;       /* 0..431 */
        int src_y = blk_y * 2;
        if (src_y + 1 >= SENSOR_PX_H) src_y = SENSOR_PX_H - 2;

        const uint8_t* row0 = frame + src_y       * byte_stride;
        const uint8_t* row1 = frame + (src_y + 1) * byte_stride;

        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)(oy + DISP_YOFF) * pitch);

        for (int ox = 0; ox < DISP_W; ox++) {
            int blk_x = (ox * SENSOR_BLK_W) / DISP_W;   /* 0..767 */
            int src_x = blk_x * 2;

            /* RGGB: (even,even)=R  (even,odd)=Gr  (odd,even)=Gb  (odd,odd)=B */
            uint8_t R  = raw10_msb8(row0, src_x);
            uint8_t Gr = raw10_msb8(row0, src_x + 1);
            uint8_t Gb = raw10_msb8(row1, src_x);
            uint8_t B  = raw10_msb8(row1, src_x + 1);

            uint8_t G = (uint8_t)(((unsigned)Gr + Gb) >> 1);

            /* Moderate brightness stretch: subtract black level, scale. */
            int r_val = ((int)R - 16) * 3;
            int g_val = ((int)G - 16) * 2;
            int b_val = ((int)B - 16) * 3;

            uint8_t rb = (r_val > 255) ? 255 : (r_val < 0 ? 0 : (uint8_t)r_val);
            uint8_t gb = (g_val > 255) ? 255 : (g_val < 0 ? 0 : (uint8_t)g_val);
            uint8_t bb = (b_val > 255) ? 255 : (b_val < 0 ? 0 : (uint8_t)b_val);

            /* BGR in the lower 24 bits matches draw_pixel convention. */
            fb_row[DISP_XOFF + ox] =
                0xFF000000u | ((uint32_t)bb << 16) | ((uint32_t)gb << 8) | (uint32_t)rb;
        }
    }

    /* Bottom letterbox (black) */
    for (int y = DISP_YOFF + DISP_H; y < FB_H; y++) {
        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)y * pitch);
        for (int x = 0; x < FB_W; x++) fb_row[x] = 0xFF000000u;
    }
}
