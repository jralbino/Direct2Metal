/* RAW10 BGGR debayer + libcamera ISP (BLC/WB/CCM/gamma) for IMX708 binned
 * 1536x864 mode. Two consumers:
 *   - debayer_raw10_to_chw320: 864×864 center crop → 320×320 CHW float32
 *     (YOLO input).
 *   - debayer_raw10_to_fb:     full 1536×864 frame → 640×360 letterboxed
 *     in the 640×480 framebuffer (display).
 * Both use the same per-pixel pipeline so YOLO and the user see identical
 * colour. Pipeline + constants ported byte-exact from camara_direct V162. */
#include <stdint.h>

extern const uint8_t* unicam_frame_ptr();
extern int            unicam_frame_w();   /* byte stride (1920) */
extern int            unicam_frame_h();   /* 864                 */

/* libcamera ISP constants — verbatim from linux_extract/tuning/imx708.json
 * (camara_direct V162). BlackLevel=64 in 10-bit → 16 in MSB8. WB Q8 from
 * AsShotNeutral [0.4784, 1.0, 0.5629]. CCM Q10 signed at 4640K daylight.
 * Gamma: 256-entry LUT built at first call from the 51-point rpi.contrast
 * curve. */
#define BLC_MSB8 16u
#define WB_R    535u
#define WB_G    256u
#define WB_B    455u
#define CCM_RR  1566
#define CCM_RG  -360
#define CCM_RB  -182
#define CCM_GR  -290
#define CCM_GG  1711
#define CCM_GB  -397
#define CCM_BR    17
#define CCM_BG  -586
#define CCM_BB  1592

static const uint16_t k_gamma_pts[51][2] = {
    {    0,     0}, {  512,  2518}, { 1024,  5033}, { 1536,  7175},
    { 2048,  9309}, { 2560, 10814}, { 3072, 12312}, { 3584, 13773},
    { 4096, 15225}, { 4608, 16566}, { 5120, 17899}, { 5632, 19221},
    { 6144, 20534}, { 6656, 21684}, { 7168, 22826}, { 7680, 24024},
    { 8192, 25212}, { 9216, 27251}, {10240, 29167}, {11264, 30947},
    {12288, 32696}, {13312, 34309}, {14336, 35849}, {15360, 37194},
    {16384, 38445}, {17408, 39598}, {18432, 40732}, {19456, 41717},
    {20480, 42687}, {22528, 44343}, {24576, 45871}, {26624, 47222},
    {28672, 48441}, {30720, 49460}, {32768, 50470}, {34816, 51476},
    {36864, 52480}, {38912, 53382}, {40960, 54294}, {43008, 55155},
    {45056, 56035}, {47104, 56920}, {49152, 57824}, {51200, 58737},
    {53248, 59666}, {55296, 60604}, {57344, 61558}, {59392, 62529},
    {61440, 63516}, {63488, 64519}, {65535, 65535},
};
static uint8_t k_gamma[256];
static bool    k_gamma_ready = false;

static void build_gamma_lut() {
    uint32_t j = 0;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t in16 = i * 257u;
        while (j + 1u < 50u && k_gamma_pts[j + 1u][0] < in16) j++;
        uint32_t x0 = k_gamma_pts[j][0],     y0 = k_gamma_pts[j][1];
        uint32_t x1 = k_gamma_pts[j + 1][0], y1 = k_gamma_pts[j + 1][1];
        uint32_t out16 = (x1 == x0) ? y0
                       : y0 + ((y1 - y0) * (in16 - x0)) / (x1 - x0);
        uint32_t out8  = (out16 + 128u) >> 8;
        if (out8 > 255u) out8 = 255u;
        k_gamma[i] = (uint8_t)out8;
    }
    k_gamma_ready = true;
}

/* Per-channel ISP step: post-pedestal R/G/B (0..255) → gamma-encoded sRGB.
 * Bit-exact equivalent of the per-row NEON inner loop of camara_direct V162. */
static inline void isp_pixel(uint8_t Rin, uint8_t Gin, uint8_t Bin,
                             uint8_t* Rout, uint8_t* Gout, uint8_t* Bout) {
    int R = (int)Rin - (int)BLC_MSB8; if (R < 0) R = 0;
    int G = (int)Gin - (int)BLC_MSB8; if (G < 0) G = 0;
    int B = (int)Bin - (int)BLC_MSB8; if (B < 0) B = 0;

    /* White balance (Q8). Widen to 32-bit before multiply — WB_R=535 over a
     * value near 255 overflows 16-bit. */
    int Rwb = (R * (int)WB_R) >> 8; if (Rwb > 255) Rwb = 255;
    int Gwb = (G * (int)WB_G) >> 8; if (Gwb > 255) Gwb = 255;
    int Bwb = (B * (int)WB_B) >> 8; if (Bwb > 255) Bwb = 255;

    /* CCM (Q10 signed) with rounding +512. */
    int Rc = (Rwb * CCM_RR + Gwb * CCM_RG + Bwb * CCM_RB + 512) >> 10;
    int Gc = (Rwb * CCM_GR + Gwb * CCM_GG + Bwb * CCM_GB + 512) >> 10;
    int Bc = (Rwb * CCM_BR + Gwb * CCM_BG + Bwb * CCM_BB + 512) >> 10;
    if (Rc < 0) Rc = 0;
    if (Rc > 255) Rc = 255;
    if (Gc < 0) Gc = 0;
    if (Gc > 255) Gc = 255;
    if (Bc < 0) Bc = 0;
    if (Bc > 255) Bc = 255;

    *Rout = k_gamma[Rc];
    *Gout = k_gamma[Gc];
    *Bout = k_gamma[Bc];
}

/* ── Sensor geometry (IMX708 2×2-binned mode, 864 output rows) ───────────── */
#define SENSOR_PX_W   1536
#define SENSOR_PX_H    864   /* V135: full sensor frame 1536×864 (aspect 16:9) */
#define SENSOR_BLK_W  (SENSOR_PX_W / 2)   /* 768 Bayer blocks horizontally   */
#define SENSOR_BLK_H  (SENSOR_PX_H / 2)   /* 432 Bayer blocks vertically     */

/* ── YOLO path: 864×864 center crop → 256×256 ─────────────────────────────── */
/* Must match YOLO_IN in kernel.cpp. V170: 320→256 to amortise v8n's heavier
 * head (3-conv cv2/cv3 stack per level). STEP_Q8 = 864/256 in Q8 = 864. */
#define OUT_W    256
#define OUT_H    256
#define CROP_X   336                      /* (1536 - 864) / 2                */
#define CROP_SZ  864
#define STEP_Q8  864                      /* 864/256 = 3.375 in Q8           */

/* ── Framebuffer path: full 1536×864 → 640×360 letterboxed on 640×480 ──── */
/* V135: native aspect 1536:864 = 16:9. At 640 wide: 640 × (864/1536) = 360.
 * Letterbox: (480 - 360) / 2 = 60 rows top/bottom. */
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

/* ── YOLO input: debayer 864×864 center crop → 320×320 CHW float32 ───────
 *
 * V162 port: full libcamera-style ISP applied so the model sees the same
 * R/G/B distribution it was trained on.
 *
 * Bayer pattern is BGGR (after the IMX708 0x0101=0x03 H+V flip programmed
 * in imx708_regs.h):
 *   (even,even)=B  (even,odd)=Gb  (odd,even)=Gr  (odd,odd)=R
 *
 * Per-pixel pipeline: gather 2×2 block → black-level subtract → WB Q8 →
 * CCM Q10 → gamma LUT → divide by 255 into float plane. */
void debayer_raw10_to_chw320(float* dst) {
    if (!k_gamma_ready) build_gamma_lut();

    const uint8_t* frame = unicam_frame_ptr();
    const int byte_stride = unicam_frame_w();
    const float inv255 = 1.0f / 255.0f;

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

        for (int ox = 0; ox < OUT_W; ox++) {
            int src_x = ((ox * STEP_Q8) >> 8) + CROP_X;
            src_x &= ~1;

            /* BGGR (post-flip): row0=B/Gb row1=Gr/R. */
            uint8_t B  = raw10_msb8(row0, src_x);
            uint8_t Gb = raw10_msb8(row0, src_x + 1);
            uint8_t Gr = raw10_msb8(row1, src_x);
            uint8_t R  = raw10_msb8(row1, src_x + 1);
            uint8_t G  = (uint8_t)(((unsigned)Gr + Gb) >> 1);

            uint8_t Ro, Go, Bo;
            isp_pixel(R, G, B, &Ro, &Go, &Bo);
            r_row[ox] = (float)Ro * inv255;
            g_row[ox] = (float)Go * inv255;
            b_row[ox] = (float)Bo * inv255;
        }
    }
}

/* Fill the top/bottom letterbox bars with solid black. The image area never
 * touches these rows, so call this once at startup and never again. */
void debayer_letterbox_clear(uint8_t* fb, uint32_t pitch) {
    for (int y = 0; y < DISP_YOFF; y++) {
        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)y * pitch);
        for (int x = 0; x < FB_W; x++) fb_row[x] = 0xFF000000u;
    }
    for (int y = DISP_YOFF + DISP_H; y < FB_H; y++) {
        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)y * pitch);
        for (int x = 0; x < FB_W; x++) fb_row[x] = 0xFF000000u;
    }
}

/* Debayer DISP rows [disp_y_start, disp_y_end) onto fb (row offset DISP_YOFF
 * applied internally). Called by both single-core debayer_raw10_to_fb and
 * the per-core worker dispatch. raw must point to the completed Unicam
 * buffer (cache invalidated). */
void debayer_raw10_to_fb_band(const uint8_t* raw, uint8_t* fb, uint32_t pitch,
                              int disp_y_start, int disp_y_end) {
    if (!k_gamma_ready) build_gamma_lut();
    const int byte_stride = 1920;   /* RAW10-packed 1536px = 1920 bytes/line */

    for (int oy = disp_y_start; oy < disp_y_end; oy++) {
        int blk_y = (oy * SENSOR_BLK_H) / DISP_H;
        int src_y = blk_y * 2;
        if (src_y + 1 >= SENSOR_PX_H) src_y = SENSOR_PX_H - 2;

        const uint8_t* row0 = raw + src_y       * byte_stride;
        const uint8_t* row1 = raw + (src_y + 1) * byte_stride;

        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)(oy + DISP_YOFF) * pitch);

        for (int ox = 0; ox < DISP_W; ox++) {
            int blk_x = (ox * SENSOR_BLK_W) / DISP_W;
            int src_x = blk_x * 2;

            /* BGGR (post-flip): row0=B/Gb, row1=Gr/R. */
            uint8_t B  = raw10_msb8(row0, src_x);
            uint8_t Gb = raw10_msb8(row0, src_x + 1);
            uint8_t Gr = raw10_msb8(row1, src_x);
            uint8_t R  = raw10_msb8(row1, src_x + 1);
            uint8_t G  = (uint8_t)(((unsigned)Gr + Gb) >> 1);

            uint8_t Ro, Go, Bo;
            isp_pixel(R, G, B, &Ro, &Go, &Bo);

            fb_row[DISP_XOFF + ox] =
                0xFF000000u | ((uint32_t)Bo << 16) | ((uint32_t)Go << 8) | (uint32_t)Ro;
        }
    }
}

/* Single-core convenience wrapper: clear letterbox + debayer full image.
 * Used by the existing camera_render_fullres call site. The multi-core
 * pipeline (parallel_debayer_*) bypasses this and dispatches bands directly. */
void debayer_raw10_to_fb(uint8_t* fb, uint32_t pitch) {
    debayer_letterbox_clear(fb, pitch);
    debayer_raw10_to_fb_band(unicam_frame_ptr(), fb, pitch, 0, DISP_H);
}

/* Thumbnail debayer — full sensor (1536×864) → thumb_w × thumb_h with the
 * same BGGR + ISP pipeline. Positions output at (x_off, y_off) in fb.
 *
 * Single-core, ~0.5–1 ms for 192×108 thumbnails. Used when we don't want to
 * spend the multi-core debayer budget on a full-res camera image. */
void debayer_raw10_to_thumbnail(const uint8_t* raw, uint8_t* fb, uint32_t pitch,
                                int x_off, int y_off, int thumb_w, int thumb_h) {
    if (!k_gamma_ready) build_gamma_lut();
    const int byte_stride = 1920;

    for (int oy = 0; oy < thumb_h; oy++) {
        int blk_y = (oy * SENSOR_BLK_H) / thumb_h;
        int src_y = blk_y * 2;
        if (src_y + 1 >= SENSOR_PX_H) src_y = SENSOR_PX_H - 2;

        const uint8_t* row0 = raw + src_y       * byte_stride;
        const uint8_t* row1 = raw + (src_y + 1) * byte_stride;

        uint32_t* fb_row = (uint32_t*)(fb + (uint32_t)(y_off + oy) * pitch);

        for (int ox = 0; ox < thumb_w; ox++) {
            int blk_x = (ox * SENSOR_BLK_W) / thumb_w;
            int src_x = blk_x * 2;

            uint8_t B  = raw10_msb8(row0, src_x);
            uint8_t Gb = raw10_msb8(row0, src_x + 1);
            uint8_t Gr = raw10_msb8(row1, src_x);
            uint8_t R  = raw10_msb8(row1, src_x + 1);
            uint8_t G  = (uint8_t)(((unsigned)Gr + Gb) >> 1);

            uint8_t Ro, Go, Bo;
            isp_pixel(R, G, B, &Ro, &Go, &Bo);

            fb_row[x_off + ox] =
                0xFF000000u | ((uint32_t)Bo << 16) | ((uint32_t)Go << 8) | (uint32_t)Ro;
        }
    }
}

