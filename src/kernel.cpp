/* File: src/kernel.cpp - RPi Zero 2 W Bare-Metal Kernel with Diagnostics */
#include <stdint.h>
#include "ops.h"
#include "model_config.h"
#include "mmu.h"
#include "multicore.h"

/* ============================================================
 * PERIPHERAL BASE: 0x3F000000 (BCM2837 / RPi Zero 2 W)
 * ============================================================ */

/* --- GPIO --- */
volatile uint32_t* const GPFSEL1    = (uint32_t*)0x3F200004;
volatile uint32_t* const GPFSEL2    = (uint32_t*)0x3F200008;
volatile uint32_t* const GPSET0     = (uint32_t*)0x3F20001C;
volatile uint32_t* const GPCLR0     = (uint32_t*)0x3F200028;
volatile uint32_t* const GPPUD      = (uint32_t*)0x3F200094;
volatile uint32_t* const GPPUDCLK0  = (uint32_t*)0x3F200098;

/* --- PL011 UART0 --- */
volatile uint32_t* const UART0_DR   = (uint32_t*)0x3F201000;
volatile uint32_t* const UART0_FR   = (uint32_t*)0x3F201018;
volatile uint32_t* const UART0_IBRD = (uint32_t*)0x3F201024;
volatile uint32_t* const UART0_FBRD = (uint32_t*)0x3F201028;
volatile uint32_t* const UART0_LCRH = (uint32_t*)0x3F20102C;
volatile uint32_t* const UART0_CR   = (uint32_t*)0x3F201030;
volatile uint32_t* const UART0_ICR  = (uint32_t*)0x3F201044;

/* --- MAILBOX --- */
volatile uint32_t* const MBOX_READ   = (uint32_t*)0x3F00B880;
volatile uint32_t* const MBOX_STATUS = (uint32_t*)0x3F00B898;
volatile uint32_t* const MBOX_WRITE  = (uint32_t*)0x3F00B8A0;
#define MBOX_FULL  0x80000000
#define MBOX_EMPTY 0x40000000

volatile unsigned int __attribute__((aligned(16))) mbox[36];

/* ============================================================
 * MEMORY / CACHE UTILITIES
 * ============================================================ */

void mem_barrier() {
    asm volatile("dsb sy" : : : "memory");
}

void cache_flush_range(volatile void* start, unsigned long size) {
    unsigned long addr = (unsigned long)start & ~63UL;
    unsigned long end  = (unsigned long)start + size;
    for (; addr < end; addr += 64) {
        asm volatile("dc civac, %0" : : "r"(addr) : "memory");
    }
    asm volatile("dsb sy" : : : "memory");
}

/* ============================================================
 * UART INIT + OUTPUT
 * ============================================================ */
void uart_init() {
    *UART0_CR = 0;

    unsigned int sel = *GPFSEL1;
    sel &= ~((7 << 12) | (7 << 15));
    sel |=  ((4 << 12) | (4 << 15));
    *GPFSEL1 = sel;

    *GPPUD = 0;
    for (volatile int i = 0; i < 150; i++) { asm volatile("nop"); }
    *GPPUDCLK0 = (1 << 14) | (1 << 15);
    for (volatile int i = 0; i < 150; i++) { asm volatile("nop"); }
    *GPPUD     = 0;
    *GPPUDCLK0 = 0;

    *UART0_ICR  = 0x7FF;
    *UART0_IBRD = 26;
    *UART0_FBRD = 3;
    *UART0_LCRH = 0x70;
    *UART0_CR   = 0x301;
}

void uart_putc(unsigned char c) {
    while (*UART0_FR & (1 << 5)) { asm volatile("nop"); }
    *UART0_DR = c;
}

void uart_puts(const char* s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_hex(unsigned int n) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        unsigned int nibble = (n >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
}

void uart_uint(unsigned int n) {
    if (n == 0) { uart_putc('0'); return; }
    char buf[12]; int len = 0;
    while (n > 0) { buf[len++] = '0' + (n % 10); n /= 10; }
    for (int i = len - 1; i >= 0; i--) uart_putc(buf[i]);
}

void uart_int(int n) {
    if (n < 0) { uart_putc('-'); uart_uint((unsigned int)-n); }
    else        uart_uint((unsigned int)n);
}

void uart_float(float f) {
    if (f < 0.0f) { uart_putc('-'); f = -f; }
    unsigned int ip = (unsigned int)f;
    uart_uint(ip);
    uart_putc('.');
    f -= (float)ip;
    for (int i = 0; i < 4; i++) {
        f *= 10.0f;
        unsigned int d = (unsigned int)f;
        uart_putc('0' + d);
        f -= (float)d;
    }
}

/* ============================================================
 * BENCHMARK UTILS (System Timer)
 * ============================================================ */

unsigned long get_timer_freq() {
    unsigned long val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

unsigned long get_timer_count() {
    unsigned long val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

void uart_dec(unsigned long n) {
    if (n == 0) { uart_putc('0'); return; }
    char buffer[20];
    int i = 0;
    while (n > 0) {
        buffer[i++] = (n % 10) + '0';
        n /= 10;
    }
    while (--i >= 0) {
        uart_putc(buffer[i]);
    }
}

/* ============================================================
 * LED
 * ============================================================ */

void led_init() {
    unsigned int sel = *GPFSEL2;
    sel &= ~(7 << 27);
    sel |=  (1 << 27);
    *GPFSEL2 = sel;
}

void led_on()  { *GPCLR0 = (1 << 29); }
void led_off() { *GPSET0 = (1 << 29); }

void panic_blink() {
    uart_puts("\n[KERNEL PANIC] System halted.\n");
    while (1) {
        led_on();  for (volatile int i = 0; i < 50000;  i++);
        led_off(); for (volatile int i = 0; i < 50000;  i++);
    }
}

void success_blink() {
    uart_puts("\n[KERNEL SUCCESS] System running.\n");
    while (1) {
        led_on();  for (volatile int i = 0; i < 1000000; i++);
        led_off(); for (volatile int i = 0; i < 1000000; i++);
    }
}

/* ============================================================
 * FRAMEBUFFER DRAWING PRIMITIVES
 * ============================================================ */

/* Set one pixel, bounds-checked.
 * Uses the unsigned comparison trick so negative coords (bbox overflow)
 * are caught by the same branch as out-of-range positives. */
static inline void fb_pixel(unsigned int* fb, int fb_w, int fb_h,
                             int x, int y, unsigned int color) {
    if ((unsigned)x < (unsigned)fb_w && (unsigned)y < (unsigned)fb_h)
        fb[y * fb_w + x] = color;
}

/* Fill a solid rectangle. */
static void fill_rect(unsigned int* fb, int fb_w, int fb_h,
                      int x0, int y0, int w, int h, unsigned int color) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            fb_pixel(fb, fb_w, fb_h, x, y, color);
}

/* Hollow rectangle, `t` pixels thick. */
static void draw_rect(unsigned int* fb, int fb_w, int fb_h,
                      int x0, int y0, int w, int h,
                      unsigned int color, int t) {
    for (int dx = 0; dx < w; dx++) {
        for (int dt = 0; dt < t; dt++) {
            fb_pixel(fb, fb_w, fb_h, x0 + dx, y0 + dt,           color); /* top    */
            fb_pixel(fb, fb_w, fb_h, x0 + dx, y0 + h - 1 - dt,   color); /* bottom */
        }
    }
    for (int dy = 0; dy < h; dy++) {
        for (int dt = 0; dt < t; dt++) {
            fb_pixel(fb, fb_w, fb_h, x0 + dt,           y0 + dy,  color); /* left   */
            fb_pixel(fb, fb_w, fb_h, x0 + w - 1 - dt,   y0 + dy,  color); /* right  */
        }
    }
}

/* Blit test_image (CHW float32 [0,1]) to framebuffer, upscaled by `scale`.
 * Pixel format: 0xAARRGGBB (RPi ARGB32). */
static void draw_image(unsigned int* fb, int fb_w, int fb_h,
                       const float* img_chw, int iw, int ih,
                       int dst_x, int dst_y, int scale) {
    for (int py = 0; py < ih * scale; py++) {
        int sy = py / scale;
        for (int px = 0; px < iw * scale; px++) {
            int sx   = px / scale;
            int ri   = (int)(img_chw[0 * iw * ih + sy * iw + sx] * 255.0f);
            int gi   = (int)(img_chw[1 * iw * ih + sy * iw + sx] * 255.0f);
            int bi   = (int)(img_chw[2 * iw * ih + sy * iw + sx] * 255.0f);
            if (ri > 255) ri = 255; else if (ri < 0) ri = 0;
            if (gi > 255) gi = 255; else if (gi < 0) gi = 0;
            if (bi > 255) bi = 255; else if (bi < 0) bi = 0;
            unsigned int pix = (0xFFu << 24) | ((unsigned)ri << 16)
                             | ((unsigned)gi <<  8) | (unsigned)bi;
            fb_pixel(fb, fb_w, fb_h, dst_x + px, dst_y + py, pix);
        }
    }
}

/* ============================================================
 * MAILBOX CALL
 * ============================================================ */

int mbox_call(unsigned char ch) {
    cache_flush_range(&mbox, sizeof(mbox));

    unsigned int bus_addr = (unsigned int)(unsigned long)&mbox | 0xC0000000;
    unsigned int r = (bus_addr & ~0xF) | (ch & 0xF);

    while (*MBOX_STATUS & MBOX_FULL) { asm volatile("nop"); }
    mem_barrier();
    *MBOX_WRITE = r;

    while (1) {
        while (*MBOX_STATUS & MBOX_EMPTY) { asm volatile("nop"); }
        mem_barrier();
        if (*MBOX_READ == r) {
            cache_flush_range(&mbox, sizeof(mbox));
            return mbox[1] == 0x80000000;
        }
    }
}

/* ============================================================
 * NEURAL NETWORK INFERENCE + BENCHMARK
 * ============================================================ */

extern "C" const float weights_start[];
extern "C" const float test_image[];

static float feat_a[L1_OUT_CH * L1_OUT_H * L1_OUT_W];
static float feat_b[L2_OUT_CH * L2_OUT_H * L2_OUT_W];
static float feat_c[L3_OUT_CH * L3_OUT_H * L3_OUT_W];
static float pool_out[L3_OUT_CH * POOL_OUT_H * POOL_OUT_W];
static float head_out[HEAD_OUT_CH * GRID_H * GRID_W];

void run_benchmark_inference() {
    unsigned long t_start, t_end, t_freq;
    t_freq = get_timer_freq();

    uart_puts("\r\n=== PERFORMANCE BENCHMARK ===\r\n");
    uart_puts("Timer Freq: "); uart_dec(t_freq); uart_puts(" Hz\r\n");

    const float* w = weights_start;
    unsigned long total_start = get_timer_count();

    /* --- LAYER 1 --- */
    uart_puts("L1 (Conv 3->16):   ");
    t_start = get_timer_count();

    parallel_conv2d(test_image, INPUT_H, INPUT_W, INPUT_C, w, L1_OUT_CH, L1_KER_SZ, L1_STRIDE, L1_PAD, feat_a);
    w += (uint32_t)L1_OUT_CH * L1_IN_CH * L1_KER_SZ * L1_KER_SZ;
    batchnorm_inplace(feat_a, L1_OUT_H * L1_OUT_W, L1_OUT_CH, w, w + L1_OUT_CH);
    w += 2 * L1_OUT_CH;
    leaky_relu_inplace(feat_a, L1_OUT_CH * L1_OUT_H * L1_OUT_W);

    t_end = get_timer_count();
    unsigned long t_l1 = (t_end - t_start) * 1000 / t_freq;
    uart_dec(t_l1); uart_puts(" ms\r\n");

    /* --- LAYER 2 --- */
    uart_puts("L2 (Conv 16->32):  ");
    t_start = get_timer_count();

    parallel_conv2d(feat_a, L1_OUT_H, L1_OUT_W, L1_OUT_CH, w, L2_OUT_CH, L2_KER_SZ, L2_STRIDE, L2_PAD, feat_b);
    w += (uint32_t)L2_OUT_CH * L2_IN_CH * L2_KER_SZ * L2_KER_SZ;
    batchnorm_inplace(feat_b, L2_OUT_H * L2_OUT_W, L2_OUT_CH, w, w + L2_OUT_CH);
    w += 2 * L2_OUT_CH;
    leaky_relu_inplace(feat_b, L2_OUT_CH * L2_OUT_H * L2_OUT_W);

    t_end = get_timer_count();
    unsigned long t_l2 = (t_end - t_start) * 1000 / t_freq;
    uart_dec(t_l2); uart_puts(" ms\r\n");

    /* --- LAYER 3 --- */
    uart_puts("L3 (Conv 32->64):  ");
    t_start = get_timer_count();

    parallel_conv2d(feat_b, L2_OUT_H, L2_OUT_W, L2_OUT_CH, w, L3_OUT_CH, L3_KER_SZ, L3_STRIDE, L3_PAD, feat_c);
    w += (uint32_t)L3_OUT_CH * L3_IN_CH * L3_KER_SZ * L3_KER_SZ;
    batchnorm_inplace(feat_c, L3_OUT_H * L3_OUT_W, L3_OUT_CH, w, w + L3_OUT_CH);
    w += 2 * L3_OUT_CH;
    leaky_relu_inplace(feat_c, L3_OUT_CH * L3_OUT_H * L3_OUT_W);

    t_end = get_timer_count();
    unsigned long t_l3 = (t_end - t_start) * 1000 / t_freq;
    uart_dec(t_l3); uart_puts(" ms\r\n");

    /* --- HEAD & DECODE --- */
    uart_puts("Head + Decode:     ");
    t_start = get_timer_count();

    maxpool2x2(feat_c, pool_out, L3_OUT_H, L3_OUT_W, L3_OUT_CH);
    conv1x1(pool_out, GRID_H, GRID_W, HEAD_IN_CH, w, w + HEAD_IN_CH * HEAD_OUT_CH, HEAD_OUT_CH, head_out);

    BBox boxes[16];
    int n = yolo_decode(head_out, GRID_H, GRID_W, 0.5f, boxes, 16);

    t_end = get_timer_count();
    unsigned long t_head = (t_end - t_start) * 1000 / t_freq;
    uart_dec(t_head); uart_puts(" ms\r\n");

    /* --- TOTAL --- */
    unsigned long total_end = get_timer_count();
    unsigned long total_time = (total_end - total_start) * 1000 / t_freq;

    uart_puts("--------------------------\r\n");
    uart_puts("TOTAL TIME:        "); uart_dec(total_time); uart_puts(" ms\r\n");
    uart_puts("FPS:               ");
    if(total_time > 0) uart_dec(1000 / total_time);
    else uart_puts("999");
    uart_puts("\r\n--------------------------\r\n");

    uart_puts("Detected: "); uart_int(n); uart_puts(" object(s)\r\n");
}

/* ============================================================
 * DIAGNOSTICS ("RAYOS X" DEL HARDWARE)
 * ============================================================ */

/* Lee el Nivel de Excepción Actual (EL0, EL1, EL2 o EL3) */
unsigned int get_current_el() {
    unsigned int el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3; 
}

/* Lee el Registro de Control del Sistema (SCTLR_EL1) */
unsigned int get_sctlr() {
    unsigned int sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return sctlr;
}

/* ============================================================
 * KERNEL MAIN
 * ============================================================ */

extern "C" void kernel_main() {

    uart_init();
    led_init();

    /* BSS is now clean.  Signal secondary cores (1-3) to leave their
     * spin loop in start.s and enter secondary_main().             */
    __atomic_store_n((int*)&bss_ready, 1, __ATOMIC_RELEASE);
    asm volatile("dsb sy" : : : "memory");
    asm volatile("sev");

    uart_puts("\r\n\r\n=== BOOT RPI ZERO 2 W (4-CORE) ===\r\n");
    
    // 1. REVISAR EN QUÉ NIVEL ESTAMOS
    uart_puts("Current EL: "); 
    uart_int(get_current_el()); 
    uart_puts("\r\n");

    // 2. REVISAR ESTADO ANTES DE LA MMU
    uart_puts("SCTLR_EL1 (Before): "); 
    uart_hex(get_sctlr()); 
    uart_puts("\r\n");

    // 3. INTENTAR ENCENDER EL TURBO
    init_mmu();

    // 4. REVISAR ESTADO DESPUÉS DE LA MMU
    unsigned int sctlr_after = get_sctlr();
    uart_puts("SCTLR_EL1 (After) : "); 
    uart_hex(sctlr_after); 
    uart_puts("\r\n");

    // Decodificar los bits críticos para ver si nos hizo caso
    uart_puts(" -> MMU     (Bit 0)  : "); 
    if (sctlr_after & (1 << 0)) uart_puts("ON\r\n"); else uart_puts("OFF\r\n");
    
    uart_puts(" -> D-Cache (Bit 2)  : "); 
    if (sctlr_after & (1 << 2)) uart_puts("ON\r\n"); else uart_puts("OFF\r\n");
    
    uart_puts(" -> I-Cache (Bit 12) : "); 
    if (sctlr_after & (1 << 12)) uart_puts("ON\r\n"); else uart_puts("OFF\r\n");

    uart_puts("----------------------------------------\r\n");

    led_on();

    /* EJECUTAR BENCHMARK */
    run_benchmark_inference();

    /* --- INICIO DE VIDEO --- */
    
    for (int i = 0; i < 36; i++) mbox[i] = 0;
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x40003;  /* GET_PHYSICAL_W_H */
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = 0;
    mbox[6] = 0;
    mbox[7] = 0;

    mbox_call(8);

    unsigned int w = mbox[5];
    unsigned int h = mbox[6];

    uart_puts("HDMI Detected: ");
    uart_hex(w); uart_puts(" x "); uart_hex(h); uart_puts("\r\n");

    if (w == 0 || h == 0) {
        uart_puts("WARN: No HDMI detected. Forcing 640x480.\r\n");
        w = 640; h = 480;
    }

    /* Allocate framebuffer */
    for (int i = 0; i < 36; i++) mbox[i] = 0;
    mbox[0]  = 21 * 4;
    mbox[1]  = 0;
    mbox[2]  = 0x48004; mbox[3]=8; mbox[4]=8; mbox[5]=w; mbox[6]=h;
    mbox[7]  = 0x48005; mbox[8]=4; mbox[9]=4; mbox[10]=32;
    mbox[11] = 0x40001; mbox[12]=8; mbox[13]=8; mbox[14]=4096; mbox[15]=0;
    mbox[16] = 0x40008; mbox[17]=4; mbox[18]=4; mbox[19]=0;
    mbox[20] = 0;

    if (mbox_call(8) && mbox[15] != 0) {
        unsigned int ptr   = mbox[14] & 0x3FFFFFFF;
        unsigned int size  = mbox[15];

        uart_puts("Framebuffer allocated: "); uart_hex(ptr); uart_puts("\r\n");

        if (ptr == 0) panic_blink();

        unsigned int* fb  = (unsigned int*)(unsigned long)ptr;
        int fb_w = (int)w;
        int fb_h = (int)h;

        /* ── 1. Dark background ──────────────────────────────── */
        fill_rect(fb, fb_w, fb_h, 0, 0, fb_w, fb_h, 0xFF1A1A2E);

        /* ── 2. Test image — 8× upscale, centred ─────────────── */
        const int SCALE  = 8;
        const int DISP_W = INPUT_W * SCALE;   /* 512 px */
        const int DISP_H = INPUT_H * SCALE;   /* 512 px */
        int img_x = (fb_w - DISP_W) / 2;     /* 704 @ 1920 */
        int img_y = (fb_h - DISP_H) / 2;     /* 284 @ 1080 */

        uart_puts("Drawing test image...\r\n");
        draw_image(fb, fb_w, fb_h,
                   test_image, INPUT_W, INPUT_H,
                   img_x, img_y, SCALE);

        /* ── 3. White border around the image ───────────────── */
        draw_rect(fb, fb_w, fb_h,
                  img_x - 2, img_y - 2, DISP_W + 4, DISP_H + 4,
                  0xFFFFFFFF, 2);

        /* ── 4. Bounding boxes ────────────────────────────────
         *  Use a lower threshold (0.15) so we see boxes even
         *  with untrained random weights (conf ≈ 0.23).
         *  head_out[] was filled by run_benchmark_inference().  */
        BBox vboxes[16];
        int  vn = yolo_decode(head_out, GRID_H, GRID_W,
                              0.15f, vboxes, 16);

        uart_puts("Boxes: "); uart_int(vn); uart_puts("\r\n");

        for (int i = 0; i < vn; i++) {
            /* Normalised [0,1] → display pixels */
            int bx = img_x + (int)((vboxes[i].x - vboxes[i].w * 0.5f) * DISP_W);
            int by = img_y + (int)((vboxes[i].y - vboxes[i].h * 0.5f) * DISP_H);
            int bw = (int)(vboxes[i].w * DISP_W);
            int bh = (int)(vboxes[i].h * DISP_H);
            draw_rect(fb, fb_w, fb_h, bx, by, bw, bh, 0xFFFF4444, 3);

            uart_puts("  ["); uart_int(i); uart_puts("] conf=");
            uart_float(vboxes[i].conf);
            uart_puts(" ("); uart_int(bx); uart_puts(","); uart_int(by);
            uart_puts(") "); uart_int(bw); uart_puts("x"); uart_int(bh);
            uart_puts("\r\n");
        }

        cache_flush_range((void*)(unsigned long)ptr, size);
        uart_puts("Frame rendered.\r\n");
        success_blink();

    } else {
        uart_puts("FAIL: GPU rejected framebuffer config.\r\n");
        panic_blink();
    }
}