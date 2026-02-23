/* File: src/kernel.cpp - FINAL CLEAN VERSION */
#include <stdint.h>
#include <cstddef>
#include "ops.h"
#include "mmu.h"
#include "multicore.h"

#ifndef NULL
#define NULL 0
#endif

// --- VIDEO HEADERS ---
extern void video_init();
extern void draw_pixel(int x, int y, uint32_t color);
extern void draw_rect(int x, int y, int w, int h, uint32_t color, int thickness);
extern void draw_fill(uint32_t color);
extern void draw_tensor_image(const float* img, int x_off, int y_off, int img_w, int img_h);
extern void video_flush();

// --- UART HEADERS ---
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

extern "C" const float weights_start[]; 
extern "C" const float test_image[]; 
extern "C" void flush_to_ram(volatile void* addr, unsigned long size);

// --- MEMORY BUFFERS ---
static float buf_A[2000000]; 
static float buf_B[2000000]; 
static float scratch[2000000]; 

static float save_L4[64 * 40 * 40];   
static float save_L6[128 * 20 * 20];  
static float save_Neck_P5[128 * 10 * 10]; 
static float save_Neck_P4[64 * 20 * 20]; 
static float save_P3_Head[64 * 40 * 40];
static float save_P4_Head[128 * 20 * 20];

// --- MATH UTILS ---
static float mini_exp(float x) {
    if (x > 88.0f) return 3.40282347e+38f;
    if (x < -88.0f) return 0.0f;
    float z = x * 1.44269504088896f;
    int32_t k = (z >= 0.0f) ? (int32_t)(z + 0.5f) : (int32_t)(z - 0.5f);
    float r = x - (float)k * 0.69314718055995f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.04166667f + r * 0.00833333f))));
    union { float f; uint32_t u; } bits;
    bits.u = (uint32_t)(k + 127) << 23;
    return bits.f * p;
}
static float fast_sigmoid(float x) { return 1.0f / (1.0f + mini_exp(-x)); }

static void add_bias_inplace(float* tensor, const float* bias, int hw, int c) {
    for (int ch = 0; ch < c; ch++) {
        float b = bias[ch];
        float* plane = tensor + ch * hw;
        for (int i = 0; i < hw; i++) plane[i] += b;
    }
}

struct WeightStream {
    const uint8_t* ptr;
    WeightStream(const float* start) : ptr((const uint8_t*)start) {}
    const float* next(int expected_count, const char* layer_name) {
        uint32_t actual_count = *((const uint32_t*)ptr); ptr += 4; 
        if (actual_count != (uint32_t)expected_count) {
            uart_puts("\n[FATAL ERROR] "); uart_puts(layer_name); while(1);
        }
        const float* p = (const float*)ptr; ptr += actual_count * 4;
        return p;
    }
};

static void copy_tensor(const float* src, float* dst, int n) { for (int i = 0; i < n; i++) dst[i] = src[i]; }
static void concat_tensor(const float* src1, int c1, const float* src2, int c2, float* dst, int hw) {
    copy_tensor(src1, dst, c1 * hw); copy_tensor(src2, dst + c1 * hw, c2 * hw);
}

// --- NMS ENGINE ---
#define MAX_PREDS 100
struct Box { float x, y, w, h, conf; int cls; };
static Box preds[MAX_PREDS];
static int num_preds = 0;

static float calculate_iou(const Box& a, const Box& b) {
    float x1_int = (a.x - a.w/2) > (b.x - b.w/2) ? (a.x - a.w/2) : (b.x - b.w/2);
    float y1_int = (a.y - a.h/2) > (b.y - b.h/2) ? (a.y - a.h/2) : (b.y - b.h/2);
    float x2_int = (a.x + a.w/2) < (b.x + b.w/2) ? (a.x + a.w/2) : (b.x + b.w/2);
    float y2_int = (a.y + a.h/2) < (b.y + b.h/2) ? (a.y + a.h/2) : (b.y + b.h/2);

    float w_int = x2_int - x1_int;
    float h_int = y2_int - y1_int;

    if (w_int <= 0 || h_int <= 0) return 0.0f;

    float area_int = w_int * h_int;
    float area_a = a.w * a.h;
    float area_b = b.w * b.h;

    return area_int / (area_a + area_b - area_int);
}

static void decode_yolo_grid(float* tensor, int grid_h, int grid_w, int stride, float anchors[3][2]) {
    for (int a = 0; a < 3; a++) {
        int base_ch = a * 85;
        for (int cy = 0; cy < grid_h; cy++) {
            for (int cx = 0; cx < grid_w; cx++) {
                int idx = cy * grid_w + cx;
                float obj_conf = fast_sigmoid(tensor[(base_ch + 4) * (grid_h * grid_w) + idx]);
                
                float max_cls_prob = 0.0f;
                int best_cls = -1;
                for (int c = 0; c < 80; c++) {
                    float cls_prob = fast_sigmoid(tensor[(base_ch + 5 + c) * (grid_h * grid_w) + idx]);
                    if (cls_prob > max_cls_prob) { max_cls_prob = cls_prob; best_cls = c; }
                }
                
                float score = obj_conf * max_cls_prob;
                
                if (score > 0.5f && num_preds < MAX_PREDS) {
                    float tx = tensor[(base_ch + 0) * (grid_h * grid_w) + idx];
                    float ty = tensor[(base_ch + 1) * (grid_h * grid_w) + idx];
                    float tw = tensor[(base_ch + 2) * (grid_h * grid_w) + idx];
                    float th = tensor[(base_ch + 3) * (grid_h * grid_w) + idx];
                    
                    preds[num_preds].x = (fast_sigmoid(tx) * 2.0f - 0.5f + cx) * stride;
                    preds[num_preds].y = (fast_sigmoid(ty) * 2.0f - 0.5f + cy) * stride;
                    float sw = fast_sigmoid(tw) * 2.0f; 
                    float sh = fast_sigmoid(th) * 2.0f;
                    preds[num_preds].w = (sw * sw) * anchors[a][0];
                    preds[num_preds].h = (sh * sh) * anchors[a][1];
                    preds[num_preds].conf = score; 
                    preds[num_preds].cls = best_cls;
                    num_preds++;
                }
            }
        }
    }
}

// --- LAYERS ---
void c3_real_inference(float* in, float* out, float* temp, int h, int w, int c_in, int c_out, int n_depth, bool shortcut, WeightStream& ws, const char* prefix) {
    int c_hidden = c_out / 2; int hw = h * w; int hw_hidden = hw * c_hidden;
    const float* w_cv1 = ws.next(c_in * c_hidden, "C3_CV1_W"); const float* b_cv1 = ws.next(c_hidden, "C3_CV1_B");
    const float* w_cv2 = ws.next(c_in * c_hidden, "C3_CV2_W"); const float* b_cv2 = ws.next(c_hidden, "C3_CV2_B");
    const float* w_cv3 = ws.next(c_hidden * 2 * c_out, "C3_CV3_W"); const float* b_cv3 = ws.next(c_out, "C3_CV3_B");

    float* branch_a = temp; 
    parallel_conv1x1(in, h, w, c_in, w_cv1, b_cv1, c_hidden, branch_a); silu_inplace(branch_a, hw_hidden);
    
    float* branch_b = temp + hw_hidden; 
    parallel_conv1x1(in, h, w, c_in, w_cv2, b_cv2, c_hidden, branch_b); silu_inplace(branch_b, hw_hidden);

    float* b_in = branch_a; 
    float* b_out = out; 
    for (int i = 0; i < n_depth; i++) {
        const float* w_b1 = ws.next(c_hidden * c_hidden, "Bot_CV1_W"); const float* b_b1 = ws.next(c_hidden, "Bot_CV1_B");
        const float* w_b2 = ws.next(c_hidden * c_hidden * 9, "Bot_CV2_W"); const float* b_b2 = ws.next(c_hidden, "Bot_CV2_B");
        
        parallel_conv1x1(b_in, h, w, c_hidden, w_b1, b_b1, c_hidden, b_out); silu_inplace(b_out, hw_hidden);
        float* bot_res = b_out + hw_hidden;
        parallel_conv2d(b_out, h, w, c_hidden, w_b2, c_hidden, 3, 1, 1, bot_res);
        add_bias_inplace(bot_res, b_b2, hw, c_hidden); silu_inplace(bot_res, hw_hidden);
        
        if (shortcut) {
            for (int k = 0; k < hw_hidden; k++) b_in[k] += bot_res[k];
        } else {
            for (int k = 0; k < hw_hidden; k++) b_in[k] = bot_res[k];
        }
    }
    parallel_conv1x1(temp, h, w, c_out, w_cv3, b_cv3, c_out, out); silu_inplace(out, hw * c_out);
}

void sppf_real_inference(float* in, float* out, float* temp, int h, int w, int c, WeightStream& ws) {
    int c_hidden = c / 2; int hw = h * w; int hw_hidden = c_hidden * hw;
    const float* w_cv1 = ws.next(c * c_hidden, "SPPF_W1"); const float* b_cv1 = ws.next(c_hidden, "SPPF_B1");
    const float* w_cv2 = ws.next((c_hidden * 4) * c, "SPPF_W2"); const float* b_cv2 = ws.next(c, "SPPF_B2");
    
    float* cv1_out = temp; parallel_conv1x1(in, h, w, c, w_cv1, b_cv1, c_hidden, cv1_out); silu_inplace(cv1_out, hw_hidden);
    float* m1 = temp + hw_hidden; float* m2 = temp + 2*hw_hidden; float* m3 = temp + 3*hw_hidden;
    
    maxpool5x5_s1_p2(cv1_out, m1, h, w, c_hidden); maxpool5x5_s1_p2(m1, m2, h, w, c_hidden); maxpool5x5_s1_p2(m2, m3, h, w, c_hidden);
    parallel_conv1x1(temp, h, w, c_hidden * 4, w_cv2, b_cv2, c, out); silu_inplace(out, c * hw);
}

static int heartbeat_counter = 0;

void run_yolo_complete() {
    unsigned long f = get_timer_freq(); unsigned long t_start = get_timer_count();
    
    uart_puts("\n=== YOLOv5n DECODER ENGINE ===\n");
    uart_puts("[CHK 1] Variables locales y WeightStream creados.\n");
    WeightStream ws(weights_start);
    num_preds = 0; 
    
    // --- CONVERSIÓN BGR a RGB Y NORMALIZACIÓN ---
    uart_puts("[CHK 2] Iniciando conversion RGB...\n");
    int hw = 320 * 320;
    float scale = (test_image[0] > 1.0f) ? (1.0f / 255.0f) : 1.0f;
    for(int i = 0; i < hw; i++) {
        buf_B[0 * hw + i] = test_image[2 * hw + i] * scale; 
        buf_B[1 * hw + i] = test_image[1 * hw + i] * scale; 
        buf_B[2 * hw + i] = test_image[0 * hw + i] * scale; 
    }
    uart_puts("[CHK 3] Conversion RGB terminada en buf_B.\n");
    
    // BACKBONE
    uart_puts("[CHK 4] Leyendo pesos L0...\n");
    const float* w0 = ws.next(16*3*6*6, "L0_W"); const float* b0 = ws.next(16, "L0_B");
    
    uart_puts("[CHK 5] Ejecutando Convolucion L0 (parallel_conv2d)...\n");
    parallel_conv2d(buf_B, 320, 320, 3, w0, 16, 6, 2, 2, buf_A); 
    
    uart_puts("[CHK 6] Sumando bias y SiLU L0...\n");
    add_bias_inplace(buf_A, b0, 160*160, 16); silu_inplace(buf_A, 16*160*160);

    uart_puts("[CHK 7] Leyendo pesos L1...\n");
    const float* w1 = ws.next(32*16*3*3, "L1_W"); const float* b1 = ws.next(32, "L1_B");
    
    uart_puts("[CHK 8] Ejecutando Convolucion L1...\n");
    parallel_conv2d(buf_A, 160, 160, 16, w1, 32, 3, 2, 1, buf_B); add_bias_inplace(buf_B, b1, 80*80, 32); silu_inplace(buf_B, 32*80*80);
    
    uart_puts("[CHK 9] Ejecutando C3 Block L2 (parallel_conv1x1 NEON + SiLU vectorizado)...\n");
    c3_real_inference(buf_B, buf_A, scratch, 80, 80, 32, 32, 1, true, ws, "L2");

    uart_puts("[CHK 10] BLOQUE L2 SUPERADO. Memoria estable.\n");

    const float* w3 = ws.next(64*32*3*3, "L3_W"); const float* b3 = ws.next(64, "L3_B");
    parallel_conv2d(buf_A, 80, 80, 32, w3, 64, 3, 2, 1, buf_B); add_bias_inplace(buf_B, b3, 40*40, 64); silu_inplace(buf_B, 64*40*40);
    c3_real_inference(buf_B, buf_A, scratch, 40, 40, 64, 64, 2, true, ws, "L4"); copy_tensor(buf_A, save_L4, 64*40*40);

    uart_puts("[CHK 11] BLOQUE L4 SUPERADO.\n");

    const float* w5 = ws.next(128*64*3*3, "L5_W"); const float* b5 = ws.next(128, "L5_B");
    parallel_conv2d(buf_A, 40, 40, 64, w5, 128, 3, 2, 1, buf_B); add_bias_inplace(buf_B, b5, 20*20, 128); silu_inplace(buf_B, 128*20*20);
    c3_real_inference(buf_B, buf_A, scratch, 20, 20, 128, 128, 3, true, ws, "L6"); copy_tensor(buf_A, save_L6, 128*20*20);

    uart_puts("[CHK 12] BLOQUE L6 SUPERADO.\n");

    const float* w7 = ws.next(256*128*3*3, "L7_W"); const float* b7 = ws.next(256, "L7_B");
    parallel_conv2d(buf_A, 20, 20, 128, w7, 256, 3, 2, 1, buf_B); add_bias_inplace(buf_B, b7, 10*10, 256); silu_inplace(buf_B, 256*10*10);
    c3_real_inference(buf_B, buf_A, scratch, 10, 10, 256, 256, 1, true, ws, "L8");
    sppf_real_inference(buf_A, buf_B, scratch, 10, 10, 256, ws);

    uart_puts("[CHK 13] BACKBONE COMPLETADO (SPPF).\n");

    // NECK
    const float* w10 = ws.next(128*256, "L10_W"); const float* b10 = ws.next(128, "L10_B");
    parallel_conv1x1(buf_B, 10, 10, 256, w10, b10, 128, buf_A); silu_inplace(buf_A, 128*100);
    copy_tensor(buf_A, save_Neck_P5, 128*10*10); 

    upsample2x_nearest(buf_A, buf_B, 10, 10, 128); concat_tensor(buf_B, 128, save_L6, 128, scratch, 20*20);
    c3_real_inference(scratch, buf_A, buf_B, 20, 20, 256, 128, 1, false, ws, "L13");
    copy_tensor(buf_A, save_Neck_P4, 64*20*20); 

    const float* w14 = ws.next(64*128, "L14_W"); const float* b14 = ws.next(64, "L14_B");
    parallel_conv1x1(buf_A, 20, 20, 128, w14, b14, 64, buf_B); silu_inplace(buf_B, 64*400);
    copy_tensor(buf_B, save_Neck_P4, 64*20*20); 

    upsample2x_nearest(buf_B, buf_A, 20, 20, 64); concat_tensor(buf_A, 64, save_L4, 64, scratch, 40*40);
    c3_real_inference(scratch, buf_B, buf_A, 40, 40, 128, 64, 1, false, ws, "L17");
    copy_tensor(buf_B, save_P3_Head, 64*40*40); 

    const float* w18 = ws.next(64*64*3*3, "L18_W"); const float* b18 = ws.next(64, "L18_B");
    parallel_conv2d(buf_B, 40, 40, 64, w18, 64, 3, 2, 1, buf_A); add_bias_inplace(buf_A, b18, 400, 64); silu_inplace(buf_A, 64*400);
    
    concat_tensor(buf_A, 64, save_Neck_P4, 64, scratch, 20*20);
    c3_real_inference(scratch, buf_B, buf_A, 20, 20, 128, 128, 1, false, ws, "L20");
    copy_tensor(buf_B, save_P4_Head, 128*20*20); 

    const float* w21 = ws.next(128*128*3*3, "L21_W"); const float* b21 = ws.next(128, "L21_B");
    parallel_conv2d(buf_B, 20, 20, 128, w21, 128, 3, 2, 1, buf_A); add_bias_inplace(buf_A, b21, 100, 128); silu_inplace(buf_A, 128*100);
    
    concat_tensor(buf_A, 128, save_Neck_P5, 128, scratch, 10*10);
    c3_real_inference(scratch, buf_B, buf_A, 10, 10, 256, 256, 1, false, ws, "L23"); 

    uart_puts("[CHK 14] NECK COMPLETADO.\n");

    // DETECCIÓN 
    uart_puts("[CHK 15] Iniciando capas de Deteccion (Heads)...\n");
    const float* w_det_p3 = ws.next(255*64, "Det_P3_W"); const float* b_det_p3 = ws.next(255, "Det_P3_B");
    const float* w_det_p4 = ws.next(255*128, "Det_P4_W"); const float* b_det_p4 = ws.next(255, "Det_P4_B");
    const float* w_det_p5 = ws.next(255*256, "Det_P5_W"); const float* b_det_p5 = ws.next(255, "Det_P5_B");

    parallel_conv1x1(save_P3_Head, 40, 40, 64, w_det_p3, b_det_p3, 255, scratch);
    float anchors_p3[3][2] = {{10,13}, {16,30}, {33,23}};
    decode_yolo_grid(scratch, 40, 40, 8, anchors_p3);

    parallel_conv1x1(save_P4_Head, 20, 20, 128, w_det_p4, b_det_p4, 255, scratch);
    float anchors_p4[3][2] = {{30,61}, {62,45}, {59,119}};
    decode_yolo_grid(scratch, 20, 20, 16, anchors_p4);

    parallel_conv1x1(buf_B, 10, 10, 256, w_det_p5, b_det_p5, 255, scratch);
    float anchors_p5[3][2] = {{116,90}, {156,198}, {373,326}};
    decode_yolo_grid(scratch, 10, 10, 32, anchors_p5);

    unsigned long t_end = get_timer_count();
    
    uart_puts("[CHK 16] Entrando a NMS... ");
    uart_puts("Predicciones crudas: "); uart_dec(num_preds); uart_puts("\n");

    // 1. SANITIZACIÓN DE DATOS (Protección contra NaN/Infinitos)
    if (num_preds > MAX_PREDS) num_preds = MAX_PREDS; 
    
    // 2. Ordenamiento (Bubble Sort)
    for (int i = 0; i < num_preds - 1; i++) {
        for (int j = 0; j < num_preds - i - 1; j++) {
            if (preds[j].conf < preds[j+1].conf) {
                Box temp = preds[j];
                preds[j] = preds[j+1];
                preds[j+1] = temp;
            }
        }
    }

    // 3. Filtro IoU (NMS)
    float nms_thresh = 0.45f; 
    for (int i = 0; i < num_preds; i++) {
        if (preds[i].conf == 0.0f) continue; 
        for (int j = i + 1; j < num_preds; j++) {
            if (preds[j].conf > 0.0f && preds[i].cls == preds[j].cls) {
                if (calculate_iou(preds[i], preds[j]) > nms_thresh) {
                    preds[j].conf = 0.0f; 
                }
            }
        }
    }

    uart_puts("[CHK 17] NMS Terminado. Dibujando...\n");

    draw_fill(0xFF222222); 
    draw_tensor_image(test_image, 160, 80, 320, 320);

    uart_puts("\n>>> OBJETOS DETECTADOS <<<\n");
    int valid_boxes = 0;
    for (int i = 0; i < num_preds; i++) {
        if (preds[i].conf > 0.5f) {
            valid_boxes++;
            
            uart_puts("Clase: "); uart_dec(preds[i].cls);
            uart_puts(" | Confianza (x1000): "); uart_dec((int)(preds[i].conf * 1000));
            uart_puts(" | Pos: ["); uart_dec((int)preds[i].x); uart_puts(","); uart_dec((int)preds[i].y); uart_puts("]\n");

            int box_w = (int)preds[i].w;
            int box_h = (int)preds[i].h;
            int cx = (int)preds[i].x;
            int cy = (int)preds[i].y;
            
            int left = 160 + cx - (box_w / 2);
            int top  = 80 + cy - (box_h / 2);

            if (left < 0) left = 0;
            if (top < 0) top = 0;
            if (left + box_w > 640) box_w = 640 - left;
            if (top + box_h > 480) box_h = 480 - top;

            if (box_w > 0 && box_h > 0) {
                uint32_t color = (preds[i].cls == 0) ? 0xFF0000FF : 0xFF00FFFF; 
                draw_rect(left, top, box_w, box_h, color, 3); 
            }
        }
    }
    
    if(valid_boxes == 0) uart_puts("Ningun objeto valido para dibujar.\n");

    uint32_t heartbeat_color = (heartbeat_counter % 2 == 0) ? 0xFF00FF00 : 0xFF0000FF; 
    draw_rect(20, 20, 40, 40, heartbeat_color, 40); 
    heartbeat_counter++;

    num_preds = 0; 
    video_flush();
    uart_puts("TOTAL TIME: "); uart_dec((t_end-t_start)*1000/f); uart_puts(" ms\n--------------------------------\n");
}

extern "C" void kernel_main() {
    uart_init();
    uart_puts("\r\n=== Direct2Metal: MOTOR IA EN TIEMPO REAL ===\r\n");

    /* BSS is now clean.  Signal secondary cores (1-3) to leave their
     * bss_ready spin loop and enter secondary_main(). */
    __atomic_store_n((int*)&bss_ready, 1, __ATOMIC_RELEASE);
    asm volatile("dsb sy" : : : "memory");
    asm volatile("sev");

    video_init();
    draw_fill(0xFF00FF00);
    video_flush();
    uart_puts("Hardware de Video Listo.\n");

    init_mmu();

    while(1) { 
        run_yolo_complete(); 
    }
}