#pragma once
#include <stdint.h>

extern void* fb_ptr;
extern uint32_t fb_pitch;

bool framebuffer_init(uint32_t width, uint32_t height, uint32_t depth);

/* Page-flip — set the y-offset (in pixels) into the virtual framebuffer
 * that scanout starts reading from. Used with virtual_height = 2 × height
 * so we can render into the offscreen half and flip atomically. */
bool framebuffer_set_offset(uint32_t y_offset);
