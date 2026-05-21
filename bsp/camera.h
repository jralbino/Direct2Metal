#pragma once
#include <stdint.h>

// Returns true on success (sensor found + streaming started).
// Returns false on QEMU / no-camera hardware (falls back to test_image).
bool camera_init();

// Frame capture moved to bsp.h::bsp_frame_acquire() (Step 3, 2026-05-20).
// Debayer-to-YOLO-tensor moved to bsp.h::debayer_raw10_to_chw_yolo().

// Render the last captured RAW10 frame directly to the framebuffer at native 16:9.
// V124: Debayers the full 1536×864 sensor frame into a 640×360 image centered
// vertically in the 640×480 framebuffer. Top/bottom 60-pixel rows are black
// letterbox bars. Must be called after bsp_frame_acquire() for the same frame.
void camera_render_fullres(uint8_t* fb, uint32_t pitch);

// V135: Display geometry (YOLO bounding-box overlay region).
// Sensor outputs 1536×864. YOLO uses 864×864 center crop (x=336..1199).
// Display: 640×360 letterboxed at y=60. YOLO crop region on display:
//   x: 336 * (640/1536) = 140 to 1200 * (640/1536) = 500 → width 360
//   y: full height 0..360 → on fb: 60..420
#define CAM_DISP_W     360    // YOLO crop width on display (864/1536 * 640)
#define CAM_DISP_H     360    // YOLO crop height on display (full image height)
#define CAM_DISP_XOFF  140    // (640 - 360) / 2
#define CAM_DISP_YOFF   60    // vertical letterbox offset

extern bool g_use_camera;
