/* File: src/camera.cpp - V154: deshift bounded to real DMA'd rows + zero stale tail (fixes DUP runaway) */
#include "camera.h"
#include <stdint.h>

extern void uart_puts(const char* s);
extern void uart_dec(int n);

extern bool   imx708_init();
extern void   imx708_stream_on();
extern uint8_t imx708_read_frame_count();
extern void   unicam_init();
extern bool   unicam_capture_frame();
extern void   unicam_print_lane_state(const char* tag);
extern void   debayer_raw10_to_chw320(float* dst);
extern void   debayer_raw10_to_fb(uint8_t* fb, uint32_t pitch);
extern void   watchdog_kick();
extern void   i2c_write_reg16(uint8_t dev_addr, uint16_t reg, uint8_t val);
extern const uint8_t* unicam_frame_ptr();

bool g_use_camera = false;

/* ── Auto-exposure ─────────────────────────────────────────────────────────
 * V162 port: closed-loop CIT control. Each call to ae_step samples a 32×32
 * grid of raw bytes from the just-captured frame (read-only, runs after the
 * Unicam DMA has been stopped by unicam_capture_frame), removes pedestal,
 * compares to AE_TARGET and pushes a new CIT through a group-hold I2C
 * transaction so high+low bytes latch atomically at the next FS.
 *
 * Pure-P, slew-capped to ±32 lines/update so the loop converges in a few
 * frames without oscillation. AE_PERIOD throttles the update to 1/4 frames
 * → lower I2C overhead and a critically-damped response. */
#define IMX708_ADDR_AE  0x1Au
#define AE_TARGET       100u   /* post-BLC mean target (mid-warm grey) */
#define AE_BLC           16u   /* BLACK_LVL >> 2 in 8-bit space */
#define AE_PERIOD         4u
#define AE_CIT_MIN       16u
#define AE_CIT_MAX     1180u   /* FLL=0x04B6=1206 → headroom 26 lines */

static uint16_t s_ae_cit   = 0x046Bu;   /* matches imx708_regs.h initial */
static uint32_t s_ae_frame = 0u;

extern "C" uint16_t imx708_ae_cit_get() { return s_ae_cit; }

static uint32_t ae_sample_mean(const uint8_t* raw) {
    uint32_t sum = 0;
    for (uint32_t y = 8u; y < 864u; y += 27u) {           /* 32 rows */
        const uint8_t* row = raw + y * 1920u;             /* RAW10 stride */
        for (uint32_t x = 16u; x < 1536u; x += 48u) {     /* 32 cols */
            uint32_t off = (x >> 2) * 5u + (x & 3u);
            sum += row[off];
        }
    }
    return sum >> 10;   /* 1024 samples → mean */
}

static void ae_step() {
    s_ae_frame++;
    if ((s_ae_frame & (AE_PERIOD - 1u)) != 0u) return;

    const uint8_t* raw = unicam_frame_ptr();
    uint32_t mean_raw = ae_sample_mean(raw);
    int32_t  mean8    = (int32_t)mean_raw - (int32_t)AE_BLC;
    if (mean8 < 0) mean8 = 0;

    int32_t error = (int32_t)AE_TARGET - mean8;
    int32_t delta = error >> 3;          /* k_p = 1/8 */
    if (delta >  32) delta =  32;
    if (delta < -32) delta = -32;

    int32_t cit = (int32_t)s_ae_cit + delta;
    if (cit < (int32_t)AE_CIT_MIN) cit = AE_CIT_MIN;
    if (cit > (int32_t)AE_CIT_MAX) cit = AE_CIT_MAX;
    if ((uint16_t)cit == s_ae_cit) return;

    s_ae_cit = (uint16_t)cit;
    /* Group-hold so high+low bytes latch atomically at next FS. */
    i2c_write_reg16(IMX708_ADDR_AE, 0x0104, 0x01);
    i2c_write_reg16(IMX708_ADDR_AE, 0x0202, (uint8_t)(s_ae_cit >> 8));
    i2c_write_reg16(IMX708_ADDR_AE, 0x0203, (uint8_t)(s_ae_cit & 0xFF));
    i2c_write_reg16(IMX708_ADDR_AE, 0x0104, 0x00);
}

static void cam_delay_ms(int ms) {
    for (volatile int j = 0; j < ms; j++)
        for (volatile int k = 0; k < 100000; k++) asm volatile("nop");
}

bool camera_init() {
    /* Step 1: IMX708 I2C init (probe + register injection) */
    if (!imx708_init()) {
        uart_puts("[CAM] IMX708 init failed\n");
        return false;
    }

    /* Step 2: Unicam1 CSI-2 receiver init (full V76 register sequence) */
    unicam_init();

    imx708_stream_on();
    cam_delay_ms(500);

    uint8_t fc = imx708_read_frame_count();
    g_use_camera = true;
    uart_puts("[CAM] V154 ready (deshift bounded + tail zeroed), frames=");
    uart_dec((int)fc);
    uart_puts("\n");
    return true;
}

void camera_capture_frame(float* dst) {
    if (unicam_capture_frame()) {
        debayer_raw10_to_chw320(dst);
        /* AE runs after the frame is captured — safe to read the completed
         * buffer. Effect lands on the next frame after the sensor processes
         * the group-hold I2C update. */
        ae_step();
    } else {
        uart_puts("[CAM] Capture failed — using zero tensor\n");
        for (int i = 0; i < (3 * 320 * 320); i++) dst[i] = 0.0f;
    }
}

bool camera_capture() {
    if (!unicam_capture_frame()) return false;
    ae_step();
    return true;
}


void camera_render_fullres(uint8_t* fb, uint32_t pitch) {
    debayer_raw10_to_fb(fb, pitch);
}
