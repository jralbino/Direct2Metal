/* File: src/kernel.cpp - CLEAN VERSION (FIXED INCLUDES & WARNINGS) */
#include <stdint.h>
#include <cstddef>          // <--- AÑADIDO: Soluciona el error de size_t
#include <arm_neon.h>
#include "ops.h"
#include "mmu.h"
#include "multicore.h"
#include "safety_config.h"
#include "watchdog.h"
#include "camera.h"
#include "mailbox.h"
#include "hud.h"

#ifndef NULL
#define NULL 0
#endif

extern void video_init(); extern void draw_pixel(int x, int y, uint32_t color);
extern void draw_rect(int x, int y, int w, int h, uint32_t color, int thickness);
extern void draw_fill(uint32_t color); extern void draw_tensor_image(const float* img, int x_off, int y_off, int img_w, int img_h);
extern void draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale);
extern void video_flush();
// Framebuffer pointer and pitch exported from video.cpp
extern unsigned char* lfb;
extern uint32_t pitch;
// Multi-core debayer (declared in multicore.h) — needs the raw frame pointer.
extern const uint8_t* unicam_frame_ptr();
extern void debayer_letterbox_clear(uint8_t* fb, uint32_t pitch);
extern "C" uint16_t imx708_ae_cit_get();
extern const char* const coco_names[80];      /* defined in hud.cpp */

volatile uint32_t* const UART0_DR = (uint32_t*)0x3F201000;
volatile uint32_t* const UART0_FR = (uint32_t*)0x3F201018;
volatile uint32_t* const UART0_CR = (uint32_t*)0x3F201030;

void uart_init() {
    *UART0_CR = 0;
    *((volatile uint32_t*)0x3F200004) = (*((volatile uint32_t*)0x3F200004) & ~((7 << 12) | (7 << 15))) | ((4 << 12) | (4 << 15));
    *((volatile uint32_t*)0x3F201044) = 0x7FF;
    *((volatile uint32_t*)0x3F201024) = 26;
    *((volatile uint32_t*)0x3F201028) = 3;
    *((volatile uint32_t*)0x3F20102C) = 0x70;
    *UART0_CR = 0x301;
}
void uart_putc(unsigned char c) { while (*UART0_FR & (1 << 5)); *UART0_DR = c; }
void uart_puts(const char* s) { while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); } }
extern "C" void uart_puts_c(const char* s) { uart_puts(s); }
void uart_dec(int n) {
    if (n < 0) { uart_putc('-'); n = -n; }
    if (n == 0) { uart_putc('0'); return; }
    char buf[20]; int i = 0;
    while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; }
    while (--i >= 0) uart_putc(buf[i]);
}

unsigned long get_timer_freq() { unsigned long v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
unsigned long get_timer_count() { unsigned long v; asm volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }

extern "C" const float weights_start[]; extern "C" const float weights_end[];
extern "C" const float test_image[]; extern "C" void flush_to_ram(volatile void* addr, unsigned long size);

/* B1 — YOLO inference resolution. Must be divisible by stride 32. 192 is
 * 2.7× cheaper than 320 in FLOPs but the model's discrimination drops a lot
 * — at 192² distant/small objects are <5×5 px in the P3 grid (stride 8).
 * Bumped back to 320 to A/B-test whether false positives were a resolution
 * issue or a model-capacity issue. fps cost: ~5.2 → ~1.9. */
#define YOLO_IN  320
#define YOLO_S2  (YOLO_IN / 2)
#define YOLO_S4  (YOLO_IN / 4)
#define YOLO_S8  (YOLO_IN / 8)
#define YOLO_S16 (YOLO_IN / 16)
#define YOLO_S32 (YOLO_IN / 32)

static float buf_A[2000000]; static float buf_B[2000000]; static float scratch[2000000];
static float cam_frame[3 * YOLO_IN * YOLO_IN];
static float save_L4[64 * YOLO_S8 * YOLO_S8];   static float save_L6[128 * YOLO_S16 * YOLO_S16];
static float save_Neck_P5[128 * YOLO_S32 * YOLO_S32];
static float save_Neck_P4[64 * YOLO_S16 * YOLO_S16];
static float save_P3_Head[64 * YOLO_S8 * YOLO_S8];
static float save_P4_Head[128 * YOLO_S16 * YOLO_S16];

static float mini_exp(float x) {
    if (x > 88.0f) { return 3.40282347e+38f; } // <--- Corregido el warning de indentación
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

static void decode_yolo_grid(float* tensor, int grid_h, int grid_w, int stride, float anchors[3][2]) {
    int grd = grid_h * grid_w;
    for (int a = 0; a < 3; a++) {
        int base_ch = a * 85;
        for (int cy = 0; cy < grid_h; cy++) {
            for (int cx = 0; cx < grid_w; cx++) {
                int idx = cy * grid_w + cx;
                float obj_conf = fast_sigmoid(tensor[(base_ch + 4) * grd + idx]);
                if (obj_conf <= OBJ_PRE_THRESH) continue;
                float max_cls_prob = 0.0f; int best_cls = -1;
                for (int c = 0; c < NUM_CLASSES; c++) {
                    float prob = fast_sigmoid(tensor[(base_ch + 5 + c) * grd + idx]);
                    if (prob > max_cls_prob) { max_cls_prob = prob; best_cls = c; }
                }
                float score = obj_conf * max_cls_prob;
                if (score > OBJ_PRE_THRESH && num_preds < MAX_PREDS) {
                    float tx = tensor[(base_ch + 0) * grd + idx]; float ty = tensor[(base_ch + 1) * grd + idx];
                    float tw = tensor[(base_ch + 2) * grd + idx]; float th = tensor[(base_ch + 3) * grd + idx];
                    preds[num_preds].x = (fast_sigmoid(tx) * 2.0f - 0.5f + cx) * stride;
                    preds[num_preds].y = (fast_sigmoid(ty) * 2.0f - 0.5f + cy) * stride;
                    float sw = fast_sigmoid(tw) * 2.0f; float sh = fast_sigmoid(th) * 2.0f;
                    preds[num_preds].w = (sw * sw) * anchors[a][0]; preds[num_preds].h = (sh * sh) * anchors[a][1];
                    preds[num_preds].conf = score; preds[num_preds].cls = best_cls; num_preds++;
                }
            }
        }
    }
}

void c3_real_inference(float* in, float* out, float* temp, int h, int w, int c_in, int c_out, int n_depth, bool shortcut, WeightStream& ws, const char* prefix) {
    int c_hidden = c_out / 2; int hw = h * w; int hw_hidden = hw * c_hidden;
    const float* w_cv1 = ws.next(c_in * c_hidden, "C3_CV1_W"); const float* b_cv1 = ws.next(c_hidden, "C3_CV1_B");
    const float* w_cv2 = ws.next(c_in * c_hidden, "C3_CV2_W"); const float* b_cv2 = ws.next(c_hidden, "C3_CV2_B");
    const float* w_cv3 = ws.next(c_hidden * 2 * c_out, "C3_CV3_W"); const float* b_cv3 = ws.next(c_out, "C3_CV3_B");

    float* branch_a = temp;
    parallel_conv1x1(in, h, w, c_in, w_cv1, b_cv1, c_hidden, true, branch_a);
    float* branch_b = temp + hw_hidden;
    parallel_conv1x1(in, h, w, c_in, w_cv2, b_cv2, c_hidden, true, branch_b);

    float* b_in = branch_a; float* b_out = out;
    for (int i = 0; i < n_depth; i++) {
        const float* w_b1 = ws.next(c_hidden * c_hidden, "Bot_CV1_W"); const float* b_b1 = ws.next(c_hidden, "Bot_CV1_B");
        const float* w_b2 = ws.next(c_hidden * c_hidden * 9, "Bot_CV2_W"); const float* b_b2 = ws.next(c_hidden, "Bot_CV2_B");

        parallel_conv1x1(b_in, h, w, c_hidden, w_b1, b_b1, c_hidden, true, b_out);
        float* bot_res = b_out + hw_hidden;
        parallel_conv2d(b_out, h, w, c_hidden, w_b2, b_b2, c_hidden, 3, 1, 1, true, bot_res);

        if (shortcut) {
            int k = 0;
            for (; k <= hw_hidden - 4; k += 4) {
                float32x4_t a = vld1q_f32(b_in + k); float32x4_t b_vec = vld1q_f32(bot_res + k);
                vst1q_f32(b_in + k, vaddq_f32(a, b_vec));
            }
            for (; k < hw_hidden; k++) b_in[k] += bot_res[k];
        } else copy_tensor(bot_res, b_in, hw_hidden);
    }
    parallel_conv1x1(temp, h, w, c_out, w_cv3, b_cv3, c_out, true, out);
}

void sppf_real_inference(float* in, float* out, float* temp, int h, int w, int c, WeightStream& ws) {
    int c_hidden = c / 2; int hw = h * w; int hw_hidden = c_hidden * hw;
    const float* w_cv1 = ws.next(c * c_hidden, "SPPF_W1"); const float* b_cv1 = ws.next(c_hidden, "SPPF_B1");
    const float* w_cv2 = ws.next((c_hidden * 4) * c, "SPPF_W2"); const float* b_cv2 = ws.next(c, "SPPF_B2");

    float* cv1_out = temp; parallel_conv1x1(in, h, w, c, w_cv1, b_cv1, c_hidden, true, cv1_out);
    float* m1 = temp + hw_hidden; float* m2 = temp + 2*hw_hidden; float* m3 = temp + 3*hw_hidden;

    maxpool5x5_s1_p2(cv1_out, m1, h, w, c_hidden); maxpool5x5_s1_p2(m1, m2, h, w, c_hidden); maxpool5x5_s1_p2(m2, m3, h, w, c_hidden);
    parallel_conv1x1(temp, h, w, c_hidden * 4, w_cv2, b_cv2, c, true, out);
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
            
            // <--- Corregidos los warnings de indentación
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
    
    WeightStream ws(weights_start); num_preds = 0;

    static bool weights_verified = false;
    if (!weights_verified && WEIGHTS_CRC32 != 0x00000000U) {
        size_t wsz = (size_t)((const uint8_t*)weights_end - (const uint8_t*)weights_start);
        uint32_t actual = crc32_sw((const uint8_t*)weights_start, wsz);
        SAFETY_ASSERT(actual == WEIGHTS_CRC32, "weights CRC mismatch");
        weights_verified = true;
    }

    if (g_use_camera) camera_capture_frame(cam_frame);
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

    const float* w0 = ws.next(16*3*6*6, "L0_W"); const float* b0 = ws.next(16, "L0_B");
    parallel_conv2d(buf_B, YOLO_IN, YOLO_IN, 3, w0, b0, 16, 6, 2, 2, true, buf_A);
    unsigned long t_l0 = get_timer_count();

    const float* w1 = ws.next(32*16*3*3, "L1_W"); const float* b1 = ws.next(32, "L1_B");
    parallel_conv2d(buf_A, YOLO_S2, YOLO_S2, 16, w1, b1, 32, 3, 2, 1, true, buf_B);
    c3_real_inference(buf_B, buf_A, scratch, YOLO_S4, YOLO_S4, 32, 32, 1, true, ws, "L2");

    const float* w3 = ws.next(64*32*3*3, "L3_W"); const float* b3 = ws.next(64, "L3_B");
    parallel_conv2d(buf_A, YOLO_S4, YOLO_S4, 32, w3, b3, 64, 3, 2, 1, true, buf_B);
    c3_real_inference(buf_B, buf_A, scratch, YOLO_S8, YOLO_S8, 64, 64, 2, true, ws, "L4");
    copy_tensor(buf_A, save_L4, 64 * YOLO_S8 * YOLO_S8);

    const float* w5 = ws.next(128*64*3*3, "L5_W"); const float* b5 = ws.next(128, "L5_B");
    parallel_conv2d(buf_A, YOLO_S8, YOLO_S8, 64, w5, b5, 128, 3, 2, 1, true, buf_B);
    c3_real_inference(buf_B, buf_A, scratch, YOLO_S16, YOLO_S16, 128, 128, 3, true, ws, "L6");
    copy_tensor(buf_A, save_L6, 128 * YOLO_S16 * YOLO_S16);

    const float* w7 = ws.next(256*128*3*3, "L7_W"); const float* b7 = ws.next(256, "L7_B");
    parallel_conv2d(buf_A, YOLO_S16, YOLO_S16, 128, w7, b7, 256, 3, 2, 1, true, buf_B);
    c3_real_inference(buf_B, buf_A, scratch, YOLO_S32, YOLO_S32, 256, 256, 1, true, ws, "L8");
    sppf_real_inference(buf_A, buf_B, scratch, YOLO_S32, YOLO_S32, 256, ws);

    unsigned long t_backbone = get_timer_count();

    const float* w10 = ws.next(128*256, "L10_W"); const float* b10 = ws.next(128, "L10_B");
    parallel_conv1x1(buf_B, YOLO_S32, YOLO_S32, 256, w10, b10, 128, true, buf_A);
    copy_tensor(buf_A, save_Neck_P5, 128 * YOLO_S32 * YOLO_S32);

    upsample2x_nearest(buf_A, buf_B, YOLO_S32, YOLO_S32, 128);
    concat_tensor(buf_B, 128, save_L6, 128, scratch, YOLO_S16 * YOLO_S16);
    c3_real_inference(scratch, buf_A, buf_B, YOLO_S16, YOLO_S16, 256, 128, 1, false, ws, "L13");
    copy_tensor(buf_A, save_Neck_P4, 64 * YOLO_S16 * YOLO_S16);

    const float* w14 = ws.next(64*128, "L14_W"); const float* b14 = ws.next(64, "L14_B");
    parallel_conv1x1(buf_A, YOLO_S16, YOLO_S16, 128, w14, b14, 64, true, buf_B);
    copy_tensor(buf_B, save_Neck_P4, 64 * YOLO_S16 * YOLO_S16);

    upsample2x_nearest(buf_B, buf_A, YOLO_S16, YOLO_S16, 64);
    concat_tensor(buf_A, 64, save_L4, 64, scratch, YOLO_S8 * YOLO_S8);
    c3_real_inference(scratch, buf_B, buf_A, YOLO_S8, YOLO_S8, 128, 64, 1, false, ws, "L17");
    copy_tensor(buf_B, save_P3_Head, 64 * YOLO_S8 * YOLO_S8);

    const float* w18 = ws.next(64*64*3*3, "L18_W"); const float* b18 = ws.next(64, "L18_B");
    parallel_conv2d(buf_B, YOLO_S8, YOLO_S8, 64, w18, b18, 64, 3, 2, 1, true, buf_A);

    concat_tensor(buf_A, 64, save_Neck_P4, 64, scratch, YOLO_S16 * YOLO_S16);
    c3_real_inference(scratch, buf_B, buf_A, YOLO_S16, YOLO_S16, 128, 128, 1, false, ws, "L20");
    copy_tensor(buf_B, save_P4_Head, 128 * YOLO_S16 * YOLO_S16);

    const float* w21 = ws.next(128*128*3*3, "L21_W"); const float* b21 = ws.next(128, "L21_B");
    parallel_conv2d(buf_B, YOLO_S16, YOLO_S16, 128, w21, b21, 128, 3, 2, 1, true, buf_A);

    concat_tensor(buf_A, 128, save_Neck_P5, 128, scratch, YOLO_S32 * YOLO_S32);
    c3_real_inference(scratch, buf_B, buf_A, YOLO_S32, YOLO_S32, 256, 256, 1, false, ws, "L23");

    unsigned long t_neck = get_timer_count();

    const float* w_det_p3 = ws.next(255*64, "Det_P3_W"); const float* b_det_p3 = ws.next(255, "Det_P3_B");
    const float* w_det_p4 = ws.next(255*128, "Det_P4_W"); const float* b_det_p4 = ws.next(255, "Det_P4_B");
    const float* w_det_p5 = ws.next(255*256, "Det_P5_W"); const float* b_det_p5 = ws.next(255, "Det_P5_B");

    parallel_conv1x1(save_P3_Head, YOLO_S8, YOLO_S8, 64, w_det_p3, b_det_p3, 255, false, scratch);
    float anchors_p3[3][2] = {{10,13}, {16,30}, {33,23}};
    decode_yolo_grid(scratch, YOLO_S8, YOLO_S8, 8, anchors_p3);

    parallel_conv1x1(save_P4_Head, YOLO_S16, YOLO_S16, 128, w_det_p4, b_det_p4, 255, false, scratch);
    float anchors_p4[3][2] = {{30,61}, {62,45}, {59,119}};
    decode_yolo_grid(scratch, YOLO_S16, YOLO_S16, 16, anchors_p4);

    parallel_conv1x1(buf_B, YOLO_S32, YOLO_S32, 256, w_det_p5, b_det_p5, 255, false, scratch);
    float anchors_p5[3][2] = {{116,90}, {156,198}, {373,326}};
    decode_yolo_grid(scratch, YOLO_S32, YOLO_S32, 32, anchors_p5);

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
        /* 1-px accent border around the thumbnail */
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
                /* Color cycle per class so different objects get different colors. */
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
                /* Label sits above the bbox if there's room, otherwise inside.
                 * Scale 2 = 16-px-tall glyphs, readable at typical viewing
                 * distance. Solid black bg per-glyph (draw_text fills bg) so
                 * the label is legible against bright bbox content. */
                int label_y = (top - 18 >= disp_yoff) ? top - 18 : top + 4;
                draw_text(left + 2, label_y, lbl, color, 0xFF000000u, 2);
            }
        }
    }
    if (valid_boxes == 0) uart_puts("[DET] none\n");
    heartbeat_counter++;

    unsigned long t_end = get_timer_count();

    /* B0: per-stage profiling. Prints once per frame so we can see where the
     * budget goes. cap → rgb prep, l0 → first conv2d, backbone → through L8,
     * neck → through L23, head → up to NMS, total → end. */
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

extern "C" void _start();
extern "C" void kernel_main() {
    uart_init(); uart_puts("\r\n=== Direct2Metal V168 (YOLO 320×320 A/B test) ===\r\n");
    hud_init();

    mbox[0] = 7 * 4; mbox[1] = 0; mbox[2] = 0x00000001; mbox[3] = 4; mbox[4] = 0; mbox[5] = 0; mbox[6] = 0;
    if (!mbox_call(MBOX_CH_PROP)) uart_puts("[GPU] ERROR: No mailbox response\n");
    *(volatile uint64_t*)0xE0 = (uint64_t)&_start; *(volatile uint64_t*)0xE8 = (uint64_t)&_start; *(volatile uint64_t*)0xF0 = (uint64_t)&_start;
    asm volatile("sev");
    extern volatile int bss_ready; bss_ready = 1; flush_to_ram((void*)&bss_ready, 4);
    asm volatile("dsb sy" : : : "memory"); asm volatile("sev");
    video_init(); draw_fill(0xFF00FF00); video_flush();
    init_mmu();
    if (!camera_init()) uart_puts("[CAM] No camera — test mode\n");
    if (get_timer_freq() != 62500000UL) watchdog_init(4000);
#ifdef SIMULATION
    for (int _sim_frame = 0; _sim_frame < 3; _sim_frame++) run_yolo_complete();
    uart_puts("[SIM] All simulation frames complete — exiting QEMU.\n");
    register uint64_t _x0 asm("x0") = 0x20026;
    register uint64_t _x1 asm("x1") = 0;
    asm volatile("hlt #0xf000" :: "r"(_x0), "r"(_x1));
    __builtin_unreachable();
#else
    while(1) run_yolo_complete();
#endif
}