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

/* Per-layer profiler (kernel.cpp, V183). prof_reset() at the start of the
 * timed graph, prof_mark("name") after each stage of interest, prof_dump()
 * after the frame's [T]/[P] lines. Up to 48 marks; extra marks are dropped. */
void prof_reset();
void prof_mark(const char* name);
void prof_dump();

/* ── SoC health / measurement validity (mailbox.cpp, V194) ────────────────
 * El grafo es compute-bound y escala lineal con el reloj (V189 lo midió:
 * ×1.6 exacto de 600 MHz a 1 GHz), así que un frame time solo es comparable
 * contra otro medido al mismo reloj y sin throttle. Leer esto es la
 * diferencia entre "el kernel mejoró 2 %" y "el SoC estaba 2 % más frío". */

/* Bits de get_throttled (tag 0x00030046). Los bajos son el estado actual;
 * los 16-19 son "ocurrió desde el boot" y son los que delatan una corrida
 * de bench que empezó limpia y se degradó a la mitad. */
#define SOC_THR_UNDERVOLT_NOW  (1u << 0)
#define SOC_THR_CAPPED_NOW     (1u << 1)
#define SOC_THR_THROTTLED_NOW  (1u << 2)
#define SOC_THR_SOFTTEMP_NOW   (1u << 3)
#define SOC_THR_UNDERVOLT_EVER (1u << 16)
#define SOC_THR_CAPPED_EVER    (1u << 17)
#define SOC_THR_THROTTLED_EVER (1u << 18)
#define SOC_THR_SOFTTEMP_EVER  (1u << 19)
#define SOC_THR_EVER_MASK      (0xFu << 16)

typedef struct {
    uint32_t throttled;      /* bitmask SOC_THR_*                          */
    uint32_t arm_hz;         /* reloj ARM medido (no el pedido en config)  */
    int      temp_mc;        /* temperatura del SoC en milésimas de °C     */
    bool     have_throttled; /* false = el firmware no soporta ese tag     */
    bool     have_clock;
    bool     have_temp;
} soc_status_t;

/* Un round-trip de mailbox con los tres tags. Devuelve false solo si la
 * GPU no respondió; los have_* dicen qué tags atendió. Requiere que la MMU
 * ya esté configurada (hace el mantenimiento de caché del buffer). */
bool soc_status_read(soc_status_t* st);

/* Imprime una línea `<tag> arm=1000MHz temp=51.3C thr=0x0`. `tag` es el
 * prefijo (p.ej. "[SOC]"); nullptr usa "[SOC]". */
void soc_status_report(const char* tag);

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
 * bsp_frame_acquire() OR bsp_frame_try_advance() returns true. Returns
 * nullptr before the first frame completes. */
const uint8_t* unicam_frame_ptr();

/* V180: non-blocking ping-pong advance. One MMIO read per call when no
 * FSI is pending. Returns true when a fresh frame has just been latched
 * into the completed buffer (caller can repaint from unicam_frame_ptr()).
 * Does NOT run AE — the outer acquire path owns that. Sim build returns
 * false (sync 4-core path is used in sim). */
bool bsp_frame_try_advance();

/* ── YOLO input geometry ─────────────────────────────────────────────────
 * V192: non-square input = full sensor field of view instead of the old
 * 864×864 centre crop (which read only 56 % of the frame width — the "zoom").
 * Both dims must be multiples of 32 (the max backbone stride). YOLOv8n is
 * fully convolutional so the same weights run at any such size; the decode
 * takes grid h/w separately. 320×192 ≈ 256² in cost. The 1536×864 (16:9)
 * sensor is isotropically resized to 320×180 and letterboxed into the
 * 320×192 tensor (6 black rows top+bottom).
 * Single source of truth: the debayer and every tool read these. */
#define YOLO_W  320
#define YOLO_H  192
#define YOLO_LETTERBOX_TOP 6          /* (192 - 180) / 2 */
#define YOLO_CONTENT_H     180        /* 864 * (320/1536) */

/* Debayer the most recent raw frame into a normalized CHW float32 tensor
 * [3][YOLO_H][YOLO_W], full FoV, letterboxed. dst must point to
 * 3*YOLO_W*YOLO_H floats. RGGB → libcamera ISP (BLC/WB/CCM/gamma). */
void debayer_raw10_to_chw_yolo(float* dst);

/* Clear the top + bottom letterbox bands of the framebuffer (rows outside
 * the centered DISP_H window) to solid black. Used before debayering a
 * fresh band when the display geometry has letterbox. */
void debayer_letterbox_clear(uint8_t* fb, uint32_t pitch);

/* ── Sensor state (camera.cpp / camera_imx708.cpp) ───────────────────────── */

/* Current AE coarse integration time (lines), as last programmed via
 * group-hold I2C. Used by the HUD for the exposure readout. */
extern "C" uint16_t imx708_ae_cit_get();
