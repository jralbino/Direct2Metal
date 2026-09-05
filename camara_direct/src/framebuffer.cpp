#include "framebuffer.h"
#include "mailbox.h"
#include "uart.h"
#include <stdint.h>

// Importante: Reflejar el 'volatile' aquí también
extern volatile uint32_t mailbox_buffer[32];
extern bool mailbox_call(uint8_t channel);

void* fb_ptr = 0;
uint32_t fb_pitch = 0;

bool framebuffer_init(uint32_t width, uint32_t height, uint32_t depth) {
    /* Single multi-tag request: SetPhys, SetVirt, SetDepth, Alloc, GetPitch.
     * Querying pitch via 0x40008 is REQUIRED — VPU pads each scan-line to
     * an internal alignment (often 256B or even larger), so the assumed
     * `width*bpp` is wrong on real hardware and causes catastrophic
     * mis-aligned writes (left/right horizontal duplication, dark bands). */
    mailbox_buffer[0]  = 26 * 4;   /* total size in bytes (26 u32 incl end tag) */
    mailbox_buffer[1]  = 0;        /* request */

    /* Set Physical Display */
    mailbox_buffer[2]  = 0x48003;
    mailbox_buffer[3]  = 8;
    mailbox_buffer[4]  = 8;
    mailbox_buffer[5]  = width;
    mailbox_buffer[6]  = height;

    /* Set Virtual Buffer — 2× height so we have room for two surfaces
     * (fb0 at y=0, fb1 at y=height) for double-buffered scanout. */
    mailbox_buffer[7]  = 0x48004;
    mailbox_buffer[8]  = 8;
    mailbox_buffer[9]  = 8;
    mailbox_buffer[10] = width;
    mailbox_buffer[11] = height * 2;

    /* Set Depth */
    mailbox_buffer[12] = 0x48005;
    mailbox_buffer[13] = 4;
    mailbox_buffer[14] = 4;
    mailbox_buffer[15] = depth;

    /* Allocate Buffer (in: 16-byte align; out[0]=fb_addr, out[1]=fb_size) */
    mailbox_buffer[16] = 0x40001;
    mailbox_buffer[17] = 8;
    mailbox_buffer[18] = 8;
    mailbox_buffer[19] = 16;
    mailbox_buffer[20] = 0;

    /* Get Pitch (out[0] = pitch in bytes, AFTER allocate) */
    mailbox_buffer[21] = 0x40008;
    mailbox_buffer[22] = 4;
    mailbox_buffer[23] = 4;
    mailbox_buffer[24] = 0;

    /* End tag */
    mailbox_buffer[25] = 0;

    if (mailbox_call(8) && mailbox_buffer[19] != 0) {
        fb_ptr   = (void*)(uintptr_t)(mailbox_buffer[19] & 0x3FFFFFFF);
        fb_pitch = mailbox_buffer[24];
        if (fb_pitch == 0) fb_pitch = width * (depth / 8); /* fallback */
        return true;
    }

    return false;
}

bool framebuffer_set_offset(uint32_t y_offset) {
    /* Tag 0x48009 SET_VIRTUAL_OFFSET. Mailbox roundtrip on Pi3 firmware
     * is ~4 ms — much wider than the ~0.67 ms HDMI vblank — so the actual
     * HVS register write inevitably lands during active scan and produces
     * a horizontal tear seam on moving content. Tearing on motion is a
     * known limitation here; tear-free flip needs either direct HVS
     * register access (no public datasheet for the addresses) or an HVS
     * vsync IRQ handler (no IRQ infrastructure in this kernel). */
    mailbox_buffer[0] = 8 * 4;
    mailbox_buffer[1] = 0;
    mailbox_buffer[2] = 0x48009;
    mailbox_buffer[3] = 8;
    mailbox_buffer[4] = 8;
    mailbox_buffer[5] = 0;        /* x offset */
    mailbox_buffer[6] = y_offset; /* y offset */
    mailbox_buffer[7] = 0;        /* end tag */
    return mailbox_call(8);
}