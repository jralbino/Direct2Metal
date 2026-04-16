/* File: src/camera.cpp - V124: RGGB Bayer fix (blue-skin → correct skin tone) */
#include "camera.h"

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
extern void watchdog_kick();
bool g_use_camera = false;

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

    /* Step 3: Print lane state BEFORE stream_on (should be all LP-11) */
    unicam_print_lane_state("[PRE-STREAM]");

    /* Step 4: Sensor stream_on — starts MIPI transmission */
    uart_puts("[CAM] Stream ON...\n");
    imx708_stream_on();

    /* Step 5: Wait for sensor to lock and start sending frames (~150ms min) */
    cam_delay_ms(500);

    /* Step 6: Diagnostics — read frame count and lane state */
    uint8_t fc = imx708_read_frame_count();
    uart_puts("[CAM] Frame count after 500ms: ");
    uart_dec((int)fc);
    uart_puts("\n");
    unicam_print_lane_state("[POST-STREAM]");

    g_use_camera = true;
    uart_puts("[CAM] Pipeline ready (V76)\n");
    return true;
}

void camera_capture_frame(float* dst) {
    if (unicam_capture_frame()) {
        debayer_raw10_to_chw320(dst);
    } else {
        uart_puts("[CAM] Capture failed — using zero tensor\n");
        for (int i = 0; i < (3 * 320 * 320); i++) dst[i] = 0.0f;
    }
}


void camera_render_fullres(uint8_t* fb, uint32_t pitch) {
    debayer_raw10_to_fb(fb, pitch);
}
