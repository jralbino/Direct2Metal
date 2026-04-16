#pragma once
#include <stdint.h>

// Returns true on success (sensor found + streaming started).
// Returns false on QEMU / no-camera hardware (falls back to test_image).
bool camera_init();

// Capture one RAW8 frame and convert to normalized float32 CHW [3][320][320].
// dst must point to 3*320*320 floats = 1,228,800 bytes (cam_frame in BSS).
// Output values are in [0.0, 1.0] (already divided by 255).
void camera_capture_frame(float* dst);

// Render the last captured RAW10 frame directly to the framebuffer at native 16:9.
// V124: Debayers the full 1536×864 sensor frame into a 640×360 image centered
// vertically in the 640×480 framebuffer. Top/bottom 60-pixel rows are black
// letterbox bars. Must be called after camera_capture_frame() for the same frame.
void camera_render_fullres(uint8_t* fb, uint32_t pitch);

// V124: Display geometry (YOLO bounding-box overlay region).
// YOLO input is the 864×864 center crop of the 1536×864 sensor frame,
// scaled to 320×320. On the framebuffer that square crop is shown at x=140..500,
// y=60..420 — a 360×360 region inside the 640×360 full-FoV image.
#define CAM_DISP_W     360    // YOLO crop displayed as 360×360 (864/1536 * 640)
#define CAM_DISP_H     360
#define CAM_DISP_XOFF  140    // (640 - 360) / 2
#define CAM_DISP_YOFF   60    // (480 - 360) / 2  — 16:9 vertical letterbox

extern bool g_use_camera;
