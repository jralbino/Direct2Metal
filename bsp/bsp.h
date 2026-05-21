/* src/bsp.h — Direct2Metal BSP consolidated interface.
 *
 * UART log / timer / cache services are BSP — the application uses them
 * but their implementation belongs in boot-level code (kernel.cpp).
 *
 * Step 1 of the BSP / application split (see GOALS.md §"BSP layering").
 * This header consolidates the loose `extern` declarations that previously
 * lived inline at the top of kernel.cpp. The underlying implementations
 * (video.cpp, camera_unicam.cpp, camera_debayer.cpp, camera.cpp) are
 * unchanged — only the surface where application code names them moved
 * into one place.
 *
 * Anything declared here is part of the BSP contract. The eventual goal is
 * to wrap these symbols behind opaque accessors (bsp_fb_get(),
 * bsp_frame_acquire(), ...) so app code stops touching raw globals like
 * `lfb`. For step 1 we just consolidate — no signature changes. */
#pragma once
#include <stdint.h>

/* ── UART log (kernel.cpp) ───────────────────────────────────────────────── */

void uart_init();
void uart_putc(unsigned char c);
void uart_puts(const char* s);
void uart_dec(int n);
extern "C" void uart_puts_c(const char* s);

/* ── Timer (kernel.cpp — ARM generic CNTPCT_EL0) ─────────────────────────── */

unsigned long get_timer_freq();
unsigned long get_timer_count();

/* ── Cache (multicore.cpp) ───────────────────────────────────────────────── */

/* Clean D-cache to PoC over [addr, addr+size). NULL/zero-size safe.
 * Required before the GPU reads a framebuffer or before another core
 * reads a tensor we just wrote. */
extern "C" void flush_to_ram(volatile void* addr, unsigned long size);

/* ── Framebuffer (video.cpp) ─────────────────────────────────────────────── */

/* Init the BCM2837 GPU framebuffer at 640×480 XRGB8888. Must be called once
 * during boot, before any draw_* / video_flush call. */
void video_init();

/* Flush the framebuffer back to RAM (clean D-cache to PoC). The GPU reads
 * directly from RAM, so without this the user sees nothing on HDMI. */
void video_flush();

/* Opaque view of the active framebuffer. Returned by bsp_fb_get(); valid
 * only after video_init(). New code should prefer this over the raw `lfb`
 * / `pitch` globals — those will be removed in a later step. */
typedef struct {
    uint8_t* pixels;   /* XRGB8888, w*h*4 bytes (pitch may differ from w*4) */
    uint32_t w, h;     /* logical dimensions (640 × 480 today) */
    uint32_t pitch;    /* bytes per row */
} bsp_fb_t;

bsp_fb_t bsp_fb_get();

/* Transitional: raw framebuffer pointer and pitch. Still exposed for the
 * hot paths (HUD, debayer-to-FB) that haven't been migrated to bsp_fb_get()
 * yet. New code should NOT name these directly. */
extern unsigned char* lfb;
extern uint32_t       pitch;

/* Primitive drawing operations. Colors are XRGB8888. */
void draw_pixel(int x, int y, uint32_t color);
void draw_rect (int x, int y, int w, int h, uint32_t color, int thickness);
void draw_fill (uint32_t color);
void draw_text (int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale);

/* Render a normalized CHW float tensor as an image at (x_off,y_off). Used
 * for the QEMU / test-image fallback path. */
void draw_tensor_image(const float* img, int x_off, int y_off, int img_w, int img_h);

/* ── Camera frame (camera_unicam.cpp / camera.cpp / camera_debayer.cpp) ──── */

/* Block until the next sensor frame is captured by the Unicam DMA, then run
 * auto-exposure (group-hold I2C for the next frame). Returns false on
 * capture failure. After a successful call, the raw frame is available via
 * unicam_frame_ptr() until the next acquire. */
bool bsp_frame_acquire();

/* Pointer to the most recently completed Unicam RAW10 frame
 * (FRAME_W × FRAME_H, packed RAW10, ~1.6 MB). Valid until the next
 * bsp_frame_acquire(). Returns nullptr before the first frame completes. */
const uint8_t* unicam_frame_ptr();

/* ── YOLO input geometry ─────────────────────────────────────────────────
 * Single source of truth for the YOLO input side. The BSP debayer reads
 * this so it can never drift from what the model consumes. App-level
 * today; will migrate to app/yolo_v8n_coco/ in Step 4. */
#define YOLO_IN 256

/* Debayer the most recent raw frame into a normalized CHW float32 tensor
 * [3][YOLO_IN][YOLO_IN] (864×864 center crop → YOLO_IN²). dst must point
 * to 3*YOLO_IN*YOLO_IN floats. RGGB → libcamera ISP (BLC/WB/CCM/gamma). */
void debayer_raw10_to_chw_yolo(float* dst);

/* Clear the top + bottom letterbox bands of the framebuffer (rows outside
 * the centered DISP_H window) to solid black. Used before debayering a
 * fresh band when the display geometry has letterbox. */
void debayer_letterbox_clear(uint8_t* fb, uint32_t pitch);

/* ── Sensor state (camera.cpp / camera_imx708.cpp) ───────────────────────── */

/* Current AE coarse integration time (lines), as last programmed via
 * group-hold I2C. Used by the HUD for the exposure readout. */
extern "C" uint16_t imx708_ae_cit_get();
