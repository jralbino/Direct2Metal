#pragma once
#include <stdint.h>

void unicam_init(void* buffer);
void unicam_capture_start();
uint32_t unicam_get_ibwp();
uint32_t unicam_get_sta();
uint32_t unicam_get_ihwin();
uint32_t unicam_get_ivwin();
uint32_t unicam_get_ibls();
uint32_t unicam_get_ibsa0();
uint32_t unicam_get_ibea0();
uint32_t unicam_get_ctrl();
uint32_t unicam_get_idi0();
uint32_t unicam_get_misc();
uint32_t unicam_get_ana();
uint32_t unicam_get_pri();
uint32_t unicam_get_clk();
uint32_t unicam_get_clt();
uint32_t unicam_get_dat0();
uint32_t unicam_get_dat1();
uint32_t unicam_get_dat2();
uint32_t unicam_get_dat3();
uint32_t unicam_get_dlt();
uint32_t unicam_get_cmp0();
uint32_t unicam_get_ictl();
uint32_t unicam_get_ista();
uint32_t unicam_get_ipipe();

/* Frame-sync primitives (V110/V121 FS-to-FS pattern).
 * wait_frame_end blocks until DMA has written the last packet of the current
 * frame (FE bit in ISTA), returning with DMA stopped (CPE=0). Call rearm()
 * to resume DMA for the next frame. */
void unicam_wait_frame_end_stop();
void unicam_rearm();

/* Peak IBWP observed during the most recent unicam_wait_frame_end_stop() call.
 * Lets the caller see how far DMA actually advanced this frame, independent
 * of where IBWP froze (which can be the start address if DMA wrapped). */
uint32_t unicam_get_ibwp_max();

/* ── Continuous-mode primitives (V158 IBSA0 rotation) ──────────────────────
 * Used by main.cpp to do libcamera-style buffer rotation: IBSA0/IBEA0 are
 * re-programmed each frame to point at the next physical buffer, and LIP
 * is strobed at every FS to commit the staged address. CPE stays on the
 * whole time (no per-frame stop). This eliminates the 18 ms snap memcpy
 * because DMA writes alternate buffers automatically.
 *
 * The two functions are intentionally narrow:
 *   - unicam_stage_dma_buffer(buf) writes IBSA0/IBEA0 with the bus alias.
 *     The new address sits in the staging register and does NOT take effect
 *     until LIP fires.
 *   - unicam_wait_fs_and_lip() blocks until the next FSI, then strobes LIP.
 *     LIP at FS commits whatever was staged into the active DMA address,
 *     so the upcoming frame is captured to the staged buffer. Returns
 *     the IBWP value sampled BEFORE the LIP — i.e. the write-pointer at
 *     the moment of FS, which equals the byte count of the just-completed
 *     frame in the buffer that was active before this LIP. */
void unicam_stage_dma_buffer(void* buf);
uint32_t unicam_wait_fs_and_lip();
