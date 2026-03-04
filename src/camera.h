#pragma once
#include <stdint.h>

// Returns true on success (sensor found + streaming started).
// Returns false on QEMU / no-camera hardware (falls back to test_image).
bool camera_init();

// Capture one RAW8 frame and convert to normalized float32 CHW [3][320][320].
// dst must point to 3*320*320 floats = 1,228,800 bytes (cam_frame in BSS).
// Output values are in [0.0, 1.0] (already divided by 255).
void camera_capture_frame(float* dst);

// Render the last captured RAW8 frame directly to the framebuffer at maximum resolution.
// Writes a 480×480 debayered image centered (x=80) in a 640×480 framebuffer.
// Left/right 80-pixel letterbox columns are filled with black.
// Must be called after camera_capture_frame() for the same frame.
void camera_render_fullres(uint8_t* fb, uint32_t pitch);

// Letterbox geometry constants (camera display region within the 640×480 framebuffer)
#define CAM_DISP_W     480
#define CAM_DISP_H     480
#define CAM_DISP_XOFF   80    // x offset of camera image in framebuffer
#define CAM_DISP_YOFF    0    // y offset of camera image in framebuffer

extern bool g_use_camera;
