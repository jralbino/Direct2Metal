/* File: src/camera.cpp - V154: deshift bounded to real DMA'd rows + zero stale tail (fixes DUP runaway) */
#include "camera.h"
#include <stdint.h>
#include "bsp.h"   /* unicam_frame_ptr */

extern void uart_puts(const char* s);
extern void uart_dec(int n);

extern bool   imx708_init();
extern void   imx708_stream_on();
extern uint8_t imx708_read_frame_count();
extern void   unicam_init();
extern bool   unicam_capture_frame();
extern bool   unicam_try_advance();
extern void   unicam_print_lane_state(const char* tag);
extern void   debayer_raw10_to_fb(uint8_t* fb, uint32_t pitch);
extern void   debayer_awb_update(const uint8_t* raw);   /* V188 grey-world AWB */
extern void   watchdog_kick();
extern void   i2c_write_reg16(uint8_t dev_addr, uint16_t reg, uint8_t val);

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

/* V188 highlight protection. A mean-only AE lets the bright half of a
 * high-contrast scene (wall, window, monitor) saturate the sensor while the
 * mean sits happily at AE_TARGET — the V162..V187 "blown whites": half the
 * sensor blocks were ≥ 250 raw. If more than AE_SAT_MAX_PCT of the sample
 * grid is at/near saturation, exposure is pushed DOWN regardless of the mean,
 * proportionally to how much is clipped. */
#define AE_SAT_LEVEL    250u
#define AE_SAT_MAX_PCT    2u    /* percent of the 1024-sample grid */

static uint16_t s_ae_cit   = 0x046Bu;   /* matches imx708_regs.h initial */
static uint32_t s_ae_frame = 0u;
static uint32_t s_ae_sat   = 0u;        /* saturated samples in the last grid */

extern "C" uint16_t imx708_ae_cit_get() { return s_ae_cit; }

static uint32_t ae_sample_mean(const uint8_t* raw) {
    uint32_t sum = 0, sat = 0;
    for (uint32_t y = 8u; y < 864u; y += 27u) {           /* 32 rows */
        const uint8_t* row = raw + y * 1920u;             /* RAW10 stride */
        for (uint32_t x = 16u; x < 1536u; x += 48u) {     /* 32 cols */
            uint32_t off = (x >> 2) * 5u + (x & 3u);
            uint32_t v = row[off];
            sum += v;
            if (v >= AE_SAT_LEVEL) sat++;
        }
    }
    s_ae_sat = sat;
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

    /* V188: highlight protection overrides the mean term while too much of
     * the grid is saturated: -16 lines at the 2 % threshold, ~-72 at 35 %,
     * up to -176 with the whole grid clipped. Deliberately outside the ±32
     * slew clamp — a burnt scene must come down within a handful of AE
     * periods. Measured 2026-09-04 with the milder -8-88·sat slope: CIT
     * 1131→707 in 15 updates with sat 35 %→17 %, i.e. correct but ~2× too
     * slow, hence this gain. */
    if (s_ae_sat * 100u > AE_SAT_MAX_PCT * 1024u) {
        int32_t down = -16 - (int32_t)((s_ae_sat * 160u) / 1024u);
        if (down < delta) delta = down;
    }

#ifdef CAPTURE_FRAME
    const bool ae_log = true;                       /* diagnostic build: every update */
#else
    const bool ae_log = ((s_ae_frame & 127u) == 0u); /* every 32 updates */
#endif
    if (ae_log) {
        uart_puts("[AE] cit="); uart_dec((int)s_ae_cit);
        uart_puts(" mean="); uart_dec((int)mean8);
        uart_puts(" sat="); uart_dec((int)s_ae_sat);
        uart_puts("/1024 d="); uart_dec((int)delta); uart_puts("\n");
    }

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

/* Step 3 (BSP/app split): single capture entry point. Block until the
 * sensor delivers a frame, then run AE so the next sensor frame reflects
 * the new exposure. The app calls debayer_raw10_to_chw_yolo() separately
 * if it wants the YOLO tensor. */
bool bsp_frame_acquire() {
    if (!unicam_capture_frame()) {
        uart_puts("[CAM] Capture failed\n");
        return false;
    }
    /* AE runs after the frame is captured — safe to read the completed
     * buffer. Effect lands on the next frame after the sensor processes
     * the group-hold I2C update. */
    ae_step();
    /* V188: AWB gains for the debayer/ISP of THIS frame (software, no I2C). */
    debayer_awb_update(unicam_frame_ptr());
    return true;
}


void camera_render_fullres(uint8_t* fb, uint32_t pitch) {
    debayer_raw10_to_fb(fb, pitch);
}

/* V180 — thin wrapper exposed via bsp.h. The unicam state machine lives
 * in camera_unicam.cpp; this just forwards. AE is intentionally NOT run
 * here — the pump path doesn't want a group-hold I2C transaction mid-
 * inference. The once-per-inference bsp_frame_acquire still drives AE. */
bool bsp_frame_try_advance() { return unicam_try_advance(); }
