/* camara_direct V158 — continuous DMA via IBSA0 rotation, snap eliminated.
 *
 * Path chosen vs the V156 baseline:
 *   V156: wait_FS-to-FS stops CPE → memcpy raw_buffer→snap (18 ms) → rearm →
 *         debayer. Cycle = frame_period(117) + snap(18) = 135 ms = 7.4 fps.
 *   V158: two raw buffers (raw_a, raw_b). At each FS, software pre-stages
 *         IBSA0/IBEA0 to point at the OTHER buffer; LIP at FS commits the
 *         new address; the previous buffer is read directly (no memcpy)
 *         while DMA fills the new one. CPE never stops. Cycle =
 *         max(frame_period(117), debayer3) = 117 ms = 8.55 fps.
 *
 * Why IBSA0 rotation instead of IBSA1/IBEA1 hardware ping-pong:
 *   The libcamera runtime dumps (linux_capture_linux/unicam_run*.txt) show
 *   that bcm2835-unicam.c never programs IBSA1/IBEA1 — instead it rewrites
 *   IBSA0 between frames (the two dumps captured at different times have
 *   *different* IBSA0 values). The "double-buffer section" comment in the
 *   parent's hardware_sim.h appears to be the parent project's guess; the
 *   BCM2837 TRM is not public, and the most plausible interpretation given
 *   libcamera's behaviour is that IBSA1/IBEA1 belong to a *second* DMA
 *   channel (e.g. embedded-data on a different VC/DT) rather than a
 *   ping-pong of the same image stream. We follow the proven libcamera
 *   pattern.
 *
 * Async debayer (cores 1-3, 3 bands of 360 rows = ~32 ms) runs concurrently
 * with the wait_FS for the next frame on core 0. The debayer's memory
 * footprint (read 14.93 MB + write 8.3 MB) overlaps with DMA's writes
 * (~128 MB/s steady-state) and with core 0's MMIO polling. The 117 ms
 * frame period leaves ~85 ms idle on cores 1-3 after the debayer finishes,
 * so contention is bounded to the debayer window.
 *
 * Memory layout:
 *   RAW_A 0x01000000 .. 0x01E3D000   (14.24 MB DMA target, ping)
 *   RAW_B 0x02000000 .. 0x02E3D000   (14.24 MB DMA target, pong)
 * Both are inside the Normal Inner-Shareable cacheable region built by
 * mmu.cpp. A `dc ivac` over the just-completed buffer is required before
 * the debayer reads it because DMA writes bypass the CPU cache.
 *
 * Sensor: Pi Camera v3 in full readout (no binning), Bayer is BGGR after the
 * 0x0101=0x03 H+V flip Linux applies. ISP constants (BLC, WB, CCM, gamma)
 * are taken from the Linux libcamera tuning file
 * (linux_extract/tuning/imx708.json) and the actual DNG:
 *   BlackLevel = 64 (10-bit)
 *   AsShotNeutral = [0.4784, 1.0, 0.5629]
 *   CCM (4640K) = the daylight matrix from imx708.json
 */
#include <stdint.h>
#include <stddef.h>
#include <arm_neon.h>
#include "mailbox.h"
#include "unicam.h"
#include "imx708.h"
#include "framebuffer.h"
#include "uart.h"
#include "bsc.h"
#include "mmu.h"

/* Per-core "alive" flag. Index by core_id (0..3). Core 0 is alive by
 * construction; cores 1-3 set their slot inside secondary_main after MMU is
 * up. Marked volatile so polling from core 0 sees the update. */
static volatile uint32_t g_core_alive[4] = { 1u, 0u, 0u, 0u };

extern "C" void secondary_entry();   /* defined in start.S */

/* Forward declaration: defined below alongside the NEON debayer body. */
static void debayer_band(uint32_t vy_start, uint32_t vy_end,
                         const uint8_t* raw, volatile uint32_t* fb,
                         uint32_t fb_stride_px);

extern "C" void secondary_main(uint32_t core_id);  /* defined after FB_H */

/* Wake cores 1-3 by writing the entry-point address into the armstub spin
 * table (Pi 3+ default armstub at base 0x0). On QEMU the cores already
 * spin in our `secondary_spin` block reading the same slots, so the same
 * write wakes them everywhere. Inline asm avoids the spurious GCC
 * array-bounds warning on literal-address stores. */
static inline void poke64(uintptr_t addr, uint64_t val) {
    __asm__ volatile("str %1, [%0]" :: "r"(addr), "r"(val) : "memory");
}

static void wake_secondary_cores() {
    uint64_t entry = (uint64_t)(uintptr_t)&secondary_entry;
    poke64(0xE0u, entry);
    poke64(0xE8u, entry);
    poke64(0xF0u, entry);
    /* Critical: with MMU on, those stores land in core 0's L1. Cores 1-3
     * still have D-cache OFF and read straight from RAM — without flushing
     * to PoC they spin forever on stale 0. All three slots fit in the same
     * 64-byte line at 0xC0, but one civac per slot is bullet-proof. */
    __asm__ volatile("dc civac, %0" :: "r"((uintptr_t)0xE0u) : "memory");
    __asm__ volatile("dc civac, %0" :: "r"((uintptr_t)0xE8u) : "memory");
    __asm__ volatile("dc civac, %0" :: "r"((uintptr_t)0xF0u) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("sev");
}

extern void* fb_ptr;
extern uint32_t fb_pitch;

#define FB_W   1920u
#define FB_H   1080u

#define SENSOR_W   4608u
#define SENSOR_H   2592u
#define RAW_STRIDE 5760u             /* 4608 px * 10 bit / 8 */
#define FRAME_BYTES (RAW_STRIDE * SENSOR_H)   /* 14,929,920 = 0xE3D000 */

/* ── Multi-core task pool (V158: cores 1-3 only) ───────────────────────────
 * Core 0 owns the Unicam state machine (stage IBSA0, wait FS+LIP, swap,
 * cache invalidate). Cores 1, 2, 3 run debayer bands. Per-frame split:
 *   core 0:    stage_buf + wait_FS_LIP + invalidate + dispatch  (~117 ms)
 *   cores 1-3: debayer 360 rows each (BAND_COUNT=3, FB_H/3 = exact 360)
 *
 * Sequencing per frame:
 *   1. core 0 calls mc_dispatch_debayer_async, which sets g_task_*
 *      and bumps g_task_epoch with RELEASE + sev.
 *   2. core 0 immediately moves on to wait_FS_LIP for the next frame.
 *   3. cores 1-3 wake from WFE, ACQUIRE the new epoch, run their band,
 *      RELEASE-increment g_task_done.
 *   4. before the *next* dispatch, core 0 calls mc_wait_debayer_done so
 *      we don't tear g_task_* on top of an in-flight task.
 *
 * Memory model: both raw buffers (raw_a, raw_b) are in Inner Shareable
 * Normal cacheable memory. SCU snooping makes any cacheable accesses
 * coherent across cores; the explicit dc ivac before debayer is the
 * piece that handles DMA's writes (which bypass the cache). FB is Normal
 * Non-cacheable → writes land in SDRAM directly; cores writing different
 * rows don't collide.
 *
 * Cache-line align the two atomics to avoid false sharing between the
 * publish (g_task_epoch) and completion (g_task_done) traffic. */
__attribute__((aligned(64))) static volatile uint32_t g_task_epoch = 0;
__attribute__((aligned(64))) static volatile uint32_t g_task_done  = 0;
static const uint8_t* g_task_raw = nullptr;
static volatile uint32_t* g_task_fb = nullptr;
static uint32_t g_task_fb_stride = 0;

#define BAND_COUNT 3u                 /* secondary cores 1, 2, 3 */
#define BAND_H (FB_H / BAND_COUNT)    /* 360 rows per core (1080/3 exact) */

static inline void band_for_secondary(uint32_t core_id,
                                      uint32_t* vy_start, uint32_t* vy_end) {
    /* core_id ∈ {1,2,3}; band index = core_id - 1. */
    uint32_t band_idx = core_id - 1u;
    *vy_start = band_idx * BAND_H;
    *vy_end   = (band_idx == BAND_COUNT - 1u) ? FB_H : ((band_idx + 1u) * BAND_H);
}

extern "C" void secondary_main(uint32_t core_id) {
    /* MMU on this core BEFORE any cacheable memory access. Without this the
     * core's D-cache stays off → every load lands at uncached memory speed,
     * the exact regression the parent project hit on Phase 6 (4199 ms vs
     * 1906 ms scalar single-core). */
    mmu_enable_this_core();
    g_core_alive[core_id & 3u] = 1u;
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("sev");

    /* Task work loop — cores 1, 2, 3 cover 3 bands of 360 rows each. */
    uint32_t local_epoch = 0;
    uint32_t vy_start, vy_end;
    band_for_secondary(core_id, &vy_start, &vy_end);
    while (1) {
        /* Wait for new task. WFE clears the local event after consuming it,
         * so we re-check the epoch each wakeup; spurious SEVs just iterate. */
        while (__atomic_load_n(&g_task_epoch, __ATOMIC_ACQUIRE) == local_epoch) {
            __asm__ volatile("wfe");
        }
        local_epoch = g_task_epoch;

        debayer_band(vy_start, vy_end,
                     g_task_raw, g_task_fb, g_task_fb_stride);

        /* Signal completion. RELEASE so the FB writes happen-before the
         * counter bump observed by core 0. */
        __atomic_fetch_add(&g_task_done, 1u, __ATOMIC_RELEASE);
        __asm__ volatile("dsb sy" ::: "memory");
        __asm__ volatile("sev");
    }
}

static void mc_dispatch_debayer_async(const uint8_t* raw, volatile uint32_t* fb,
                                      uint32_t fb_stride_px) {
    /* Publish task data — set globals first, then bump epoch with RELEASE.
     * Secondaries reading epoch with ACQUIRE see consistent g_task_raw etc.
     * Returns immediately; core 0 moves on to wait_FS for the next frame
     * while cores 1-3 process this debayer task in the background.
     *
     * Caller must mc_wait_debayer_done() before any subsequent dispatch
     * (so secondaries don't tear on overwritten globals) and before
     * reusing this raw buffer as the DMA target (so we don't have DMA
     * write to a buffer cores 1-3 are still reading). */
    g_task_raw       = raw;
    g_task_fb        = fb;
    g_task_fb_stride = fb_stride_px;
    __atomic_store_n(&g_task_done, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_task_epoch, g_task_epoch + 1u, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("sev");
}

static void mc_wait_debayer_done() {
    /* Polling (not WFE) so we don't deadlock if a secondary hangs.
     * Healthy 3-core debayer is ~32 ms; timeout is generous (~1 s). */
    const uint32_t timeout_iters = 200000000u;
    uint32_t spins = 0;
    while (__atomic_load_n(&g_task_done, __ATOMIC_ACQUIRE) < BAND_COUNT) {
        if (++spins > timeout_iters) {
            uart_puts("WARN: debayer sync timeout, done=");
            uart_dec(__atomic_load_n(&g_task_done, __ATOMIC_RELAXED));
            uart_puts("\n");
            break;
        }
    }
}

/* Memory layout (RAM is identity-mapped Normal WB, Inner Shareable):
 *   RAW_A  0x01000000 .. 0x01E3D000   (14.24 MB Unicam DMA target, ping)
 *   RAW_B  0x02000000 .. 0x02E3D000   (14.24 MB Unicam DMA target, pong)
 * Both are well below the VPU heap (0x1C000000) and the framebuffer
 * (0x1E402000). */
#define RAW_A_ADDR  0x01000000UL
#define RAW_B_ADDR  0x02000000UL

/* WB extracted from libcamera DNG (AsShotNeutral = [0.4784, 1.0, 0.5629]):
 *   R*=1/0.4784 = 2.090,  G*=1.000,  B*=1/0.5629 = 1.778. Q8 fixed-point. */
#define WB_R  535u
#define WB_G  256u
#define WB_B  455u
/* DNG BlackLevel = 64 in 10-bit space; pedestal in MSB8 space = 16. */
#define BLACK_LVL 64u

/* CCM at CT=4640K (daylight) — imx708.json:rpi.ccm. Q10 signed. */
#define CCM_RR  1566
#define CCM_RG  -360
#define CCM_RB  -182
#define CCM_GR  -290
#define CCM_GG  1711
#define CCM_GB  -397
#define CCM_BR    17
#define CCM_BG  -586
#define CCM_BB  1592

/* sRGB encoding gamma LUT (close approximation of imx708.json gamma_curve). */
static const uint8_t k_gamma_srgb[256] = {
      0,  13,  22,  28,  34,  38,  42,  46,  50,  53,  56,  59,  61,  64,  66,  69,
     71,  73,  75,  77,  79,  81,  83,  85,  86,  88,  90,  92,  93,  95,  96,  98,
     99, 101, 102, 104, 105, 106, 108, 109, 110, 112, 113, 114, 115, 117, 118, 119,
    120, 121, 122, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136,
    137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 148, 149, 150, 151,
    152, 153, 154, 155, 155, 156, 157, 158, 159, 159, 160, 161, 162, 163, 163, 164,
    165, 166, 167, 167, 168, 169, 170, 170, 171, 172, 173, 173, 174, 175, 175, 176,
    177, 178, 178, 179, 180, 180, 181, 182, 182, 183, 184, 185, 185, 186, 187, 187,
    188, 189, 189, 190, 190, 191, 192, 192, 193, 194, 194, 195, 196, 196, 197, 197,
    198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205, 205, 206, 206, 207,
    208, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216,
    216, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224,
    225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231, 232, 232, 233,
    233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 238, 239, 239, 240, 240,
    241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 246, 247, 247, 248,
    248, 249, 249, 250, 250, 251, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255,
};

/* Pre-DMA fill so the FB never displays uninitialised SDRAM during cold
 * boot. Any non-zero pattern works; 0xA5 stays distinctive in hex dumps. */
#define CANARY_BYTE 0xA5u

static inline void dcache_invalidate_range(void* addr, uint32_t len) {
    const uintptr_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end   = (((uintptr_t)addr + len) + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("dc ivac, %0" :: "r"((void*)p));
    __asm__ volatile("dsb ish\nisb\n");
}

static inline void dcache_clean_range(void* addr, uint32_t len) {
    const uintptr_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end   = (((uintptr_t)addr + len) + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("dc civac, %0" :: "r"((void*)p));
    __asm__ volatile("dsb ish\nisb\n");
}

/* ── Debayer 4608x2592 BGGR → 1920x1080 ─────────────────────────────────────
 *
 * Sensor → display ratio is exactly 2.4× both axes (16:9 → 16:9), so the full
 * sensor maps to the full screen with no letterbox. For output pixel (vx,vy):
 *   blk_x = (vx * 2304) / 1920 = (vx * 6) / 5
 *   blk_y = (vy * 1296) / 1080 = (vy * 6) / 5
 *
 * Bayer extraction with 0x0101=0x03 flip applied (BGGR per DNG):
 *   row even, col even → B    row even, col odd  → Gb
 *   row odd,  col even → Gr   row odd,  col odd  → R
 *
 * The 32-bit divide and multiply chain in the hot loop is replaced with two
 * precomputed LUTs (g_row_off, g_byte_off) so the inner pixel cost drops to
 * 4 byte loads + the WB+CCM+gamma math. Bit-exact same output. */

static uint32_t g_row_off[FB_H];     /* sy_even * RAW_STRIDE for each output line */
static uint32_t g_byte_off[FB_W];    /* byte offset of B inside the row for each output column */

static void debayer_init_tables() {
    for (uint32_t vy = 0; vy < FB_H; vy++) {
        uint32_t blk_y = (vy * 6u) / 5u;
        g_row_off[vy] = (blk_y * 2u) * RAW_STRIDE;
    }
    for (uint32_t vx = 0; vx < FB_W; vx++) {
        uint32_t blk_x   = (vx * 6u) / 5u;
        uint32_t sx_even = blk_x * 2u;
        g_byte_off[vx] = (sx_even >> 2) * 5u + (sx_even & 3u);
    }
}

/* NEON-vectorised inner loop: 4 output pixels per iteration. The byte gather
 * stays scalar (4 RAW10 byte offsets per iter, ARMv8-A has no gather). The
 * pedestal subtract, white balance, CCM (3 mac chains with rounding shift),
 * and 0..255 clamp run on int16x4/int32x4 NEON. The sRGB gamma stays scalar
 * because vqtbl4q only covers 64-entry tables (ours is 256).
 *
 * Bit-exact equivalent of the scalar version: vqsub_u16 == saturating sub,
 * vshrq_n_s32 is arithmetic shift right (matching `int32 >> 10`), vmlal_n_s16
 * accumulates into int32 with no intermediate truncation. */
static void debayer_band(uint32_t vy_start, uint32_t vy_end,
                         const uint8_t* raw, volatile uint32_t* fb,
                         uint32_t fb_stride_px) {
    const uint16x4_t v_ped   = vdup_n_u16((uint8_t)(BLACK_LVL >> 2));
    const int32x4_t  v_zero  = vdupq_n_s32(0);
    const int32x4_t  v_max   = vdupq_n_s32(255);
    const int32x4_t  v_round = vdupq_n_s32(512);

    for (uint32_t vy = vy_start; vy < vy_end; vy++) {
        const uint8_t* row_e = raw + g_row_off[vy];
        const uint8_t* row_o = row_e + RAW_STRIDE;
        volatile uint32_t* fb_row = fb + vy * fb_stride_px;

        for (uint32_t vx = 0; vx < FB_W; vx += 4u) {
            uint32_t o0 = g_byte_off[vx + 0];
            uint32_t o1 = g_byte_off[vx + 1];
            uint32_t o2 = g_byte_off[vx + 2];
            uint32_t o3 = g_byte_off[vx + 3];

            uint16_t b_arr[4]  = { row_e[o0],     row_e[o1],     row_e[o2],     row_e[o3]     };
            uint16_t gb_arr[4] = { row_e[o0 + 1], row_e[o1 + 1], row_e[o2 + 1], row_e[o3 + 1] };
            uint16_t gr_arr[4] = { row_o[o0],     row_o[o1],     row_o[o2],     row_o[o3]     };
            uint16_t r_arr[4]  = { row_o[o0 + 1], row_o[o1 + 1], row_o[o2 + 1], row_o[o3 + 1] };

            uint16x4_t B  = vqsub_u16(vld1_u16(b_arr),  v_ped);
            uint16x4_t Gb = vqsub_u16(vld1_u16(gb_arr), v_ped);
            uint16x4_t Gr = vqsub_u16(vld1_u16(gr_arr), v_ped);
            uint16x4_t R  = vqsub_u16(vld1_u16(r_arr),  v_ped);
            uint16x4_t G  = vshr_n_u16(vadd_u16(Gr, Gb), 1);

            /* WB: must widen to u32 BEFORE multiply — WB_R=535, WB_B=455 push
             * 8-bit values past u16 max (255*535=136425), so vmul_n_u16 would
             * wrap and corrupt highlights into fluorescent green. */
            const uint32x4_t v_255_32 = vdupq_n_u32(255);
            uint32x4_t Rwb32 = vminq_u32(vshrq_n_u32(vmulq_n_u32(vmovl_u16(R), WB_R), 8), v_255_32);
            uint32x4_t Gwb32 = vminq_u32(vshrq_n_u32(vmulq_n_u32(vmovl_u16(G), WB_G), 8), v_255_32);
            uint32x4_t Bwb32 = vminq_u32(vshrq_n_u32(vmulq_n_u32(vmovl_u16(B), WB_B), 8), v_255_32);

            int16x4_t Rs = vreinterpret_s16_u16(vmovn_u32(Rwb32));
            int16x4_t Gs = vreinterpret_s16_u16(vmovn_u32(Gwb32));
            int16x4_t Bs = vreinterpret_s16_u16(vmovn_u32(Bwb32));

            int32x4_t r2 = v_round;
            r2 = vmlal_n_s16(r2, Rs, (int16_t)CCM_RR);
            r2 = vmlal_n_s16(r2, Gs, (int16_t)CCM_RG);
            r2 = vmlal_n_s16(r2, Bs, (int16_t)CCM_RB);
            r2 = vminq_s32(vmaxq_s32(vshrq_n_s32(r2, 10), v_zero), v_max);

            int32x4_t g2 = v_round;
            g2 = vmlal_n_s16(g2, Rs, (int16_t)CCM_GR);
            g2 = vmlal_n_s16(g2, Gs, (int16_t)CCM_GG);
            g2 = vmlal_n_s16(g2, Bs, (int16_t)CCM_GB);
            g2 = vminq_s32(vmaxq_s32(vshrq_n_s32(g2, 10), v_zero), v_max);

            int32x4_t b2 = v_round;
            b2 = vmlal_n_s16(b2, Rs, (int16_t)CCM_BR);
            b2 = vmlal_n_s16(b2, Gs, (int16_t)CCM_BG);
            b2 = vmlal_n_s16(b2, Bs, (int16_t)CCM_BB);
            b2 = vminq_s32(vmaxq_s32(vshrq_n_s32(b2, 10), v_zero), v_max);

            uint32_t r0i = (uint32_t)vgetq_lane_s32(r2, 0);
            uint32_t r1i = (uint32_t)vgetq_lane_s32(r2, 1);
            uint32_t r2i = (uint32_t)vgetq_lane_s32(r2, 2);
            uint32_t r3i = (uint32_t)vgetq_lane_s32(r2, 3);
            uint32_t g0i = (uint32_t)vgetq_lane_s32(g2, 0);
            uint32_t g1i = (uint32_t)vgetq_lane_s32(g2, 1);
            uint32_t g2i = (uint32_t)vgetq_lane_s32(g2, 2);
            uint32_t g3i = (uint32_t)vgetq_lane_s32(g2, 3);
            uint32_t b0i = (uint32_t)vgetq_lane_s32(b2, 0);
            uint32_t b1i = (uint32_t)vgetq_lane_s32(b2, 1);
            uint32_t b2i = (uint32_t)vgetq_lane_s32(b2, 2);
            uint32_t b3i = (uint32_t)vgetq_lane_s32(b2, 3);

            fb_row[vx + 0] = 0xFF000000u | (k_gamma_srgb[r0i] << 16) | (k_gamma_srgb[g0i] << 8) | k_gamma_srgb[b0i];
            fb_row[vx + 1] = 0xFF000000u | (k_gamma_srgb[r1i] << 16) | (k_gamma_srgb[g1i] << 8) | k_gamma_srgb[b1i];
            fb_row[vx + 2] = 0xFF000000u | (k_gamma_srgb[r2i] << 16) | (k_gamma_srgb[g2i] << 8) | k_gamma_srgb[b2i];
            fb_row[vx + 3] = 0xFF000000u | (k_gamma_srgb[r3i] << 16) | (k_gamma_srgb[g3i] << 8) | k_gamma_srgb[b3i];
        }
    }
}

/* ── Cycle counter (CNTPCT_EL0) for per-phase wall-clock timing. ────────── */
static uint64_t s_cnt_per_ms = 19200;   /* set from CNTFRQ_EL0 at boot */

static inline uint64_t cnt_now() {
    uint64_t v;
    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint32_t cnt_to_ms(uint64_t delta) {
    return (uint32_t)(delta / s_cnt_per_ms);
}

extern "C" void kernel_main() {
    uart_init();
    uart_puts("camara_direct V158: IMX708 4608x2592 -> HDMI 1920x1080 (continuous DMA, IBSA0 rotation)\n");

    {
        uint64_t freq;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        if (freq) s_cnt_per_ms = freq / 1000u;
    }

    debayer_init_tables();

    /* Wake cores 1-3 — each runs mmu_enable_this_core(), reports alive,
     * then enters its task-pool work loop waiting on g_task_epoch. The
     * main loop's mc_dispatch_debayer_async publishes a task per frame. */
    wake_secondary_cores();
    /* Brief poll loop: each core takes microseconds to reach the alive flag,
     * but we wait up to ~50 ms to be safe across QEMU and real hardware. */
    for (volatile uint32_t i = 0; i < 5000000u; i++) {
        if (g_core_alive[1] && g_core_alive[2] && g_core_alive[3]) break;
    }
    uart_puts("CORES alive=[");
    for (uint32_t c = 0; c < 4u; c++) {
        uart_dec(g_core_alive[c]);
        if (c < 3u) uart_putc(',');
    }
    uart_puts("]\n");

    if (!framebuffer_init(FB_W, FB_H, 32)) {
        uart_puts("FB: FAILED\n");
        return;
    }
    volatile uint32_t* fb = (volatile uint32_t*)fb_ptr;
    const uint32_t fb_stride_px = fb_pitch / 4;
    uart_puts("FB: stride_px=0x"); uart_hex(fb_stride_px);
    uart_puts(" expected=0x"); uart_hex(FB_W); uart_puts("\n");

    /* Startup fill: solid red (XRGB8888). */
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < FB_W; x++)
            fb[y * fb_stride_px + x] = 0xFF000000u | (0xFFu << 16);
    uart_puts("FB: painted red (XRGB)\n");

    if (!mailbox_set_domain_state(14, 1)) {
        uart_puts("ERR: domain 14 power on failed\n");
        return;
    }
    uart_puts("Mailbox: domain 14 ON\n");
    mailbox_set_clock_rate(4, 250000000);

    uart_puts("BSC1: init\n");
    bsc_init();

    uint8_t* raw_a = (uint8_t*)RAW_A_ADDR;
    uint8_t* raw_b = (uint8_t*)RAW_B_ADDR;

    /* Canary fill BOTH buffers BEFORE Unicam init — DMA isn't writing yet.
     * dc civac pushes the canary into RAM and invalidates cache lines so
     * the dc ivac in the main loop won't discard live DMA-written data. */
    for (uint32_t i = 0; i < FRAME_BYTES; i++) raw_a[i] = (uint8_t)CANARY_BYTE;
    for (uint32_t i = 0; i < FRAME_BYTES; i++) raw_b[i] = (uint8_t)CANARY_BYTE;
    dcache_clean_range(raw_a, FRAME_BYTES);
    dcache_clean_range(raw_b, FRAME_BYTES);

    /* Unicam init programs IBSA0/IBEA0 = raw_a and strobes LIP. After this
     * the active DMA shadow points at raw_a. */
    unicam_init(raw_a);

    if (!imx708_probe()) {
        uart_puts("ERR: IMX708 not found\n");
        return;
    }
    imx708_init_baseline();

    imx708_stream_on();
    uart_puts("IMX708: stream ON\n");
    unicam_capture_start();

    /* One-shot Linux-comparable register snapshot. Done before entering the
     * steady-state loop so the values reflect the post-init / pre-stream
     * state and can be diff'd against linux_capture_linux/. After the first
     * iteration's wait_FS_LIP runs, the rows_max value will reflect a real
     * captured frame's coverage. */
    {
        uart_puts("UNICAM_BEGIN\n");
        uart_puts("  CTRL  = 0x"); uart_hex(unicam_get_ctrl()); uart_puts("\n");
        uart_puts("  ANA   = 0x"); uart_hex(unicam_get_ana());  uart_puts("\n");
        uart_puts("  PRI   = 0x"); uart_hex(unicam_get_pri());  uart_puts("\n");
        uart_puts("  CLK   = 0x"); uart_hex(unicam_get_clk());  uart_puts("\n");
        uart_puts("  CLT   = 0x"); uart_hex(unicam_get_clt());  uart_puts("\n");
        uart_puts("  DAT0  = 0x"); uart_hex(unicam_get_dat0()); uart_puts("\n");
        uart_puts("  DAT1  = 0x"); uart_hex(unicam_get_dat1()); uart_puts("\n");
        uart_puts("  DAT2  = 0x"); uart_hex(unicam_get_dat2()); uart_puts("\n");
        uart_puts("  DAT3  = 0x"); uart_hex(unicam_get_dat3()); uart_puts("\n");
        uart_puts("  DLT   = 0x"); uart_hex(unicam_get_dlt());  uart_puts("\n");
        uart_puts("  CMP0  = 0x"); uart_hex(unicam_get_cmp0()); uart_puts("\n");
        uart_puts("  ICTL  = 0x"); uart_hex(unicam_get_ictl()); uart_puts("\n");
        uart_puts("  IDI0  = 0x"); uart_hex(unicam_get_idi0()); uart_puts("\n");
        uart_puts("  IPIPE = 0x"); uart_hex(unicam_get_ipipe());uart_puts("\n");
        uart_puts("  IBSA0 = 0x"); uart_hex(unicam_get_ibsa0());uart_puts("\n");
        uart_puts("  IBEA0 = 0x"); uart_hex(unicam_get_ibea0());uart_puts("\n");
        uart_puts("  IBLS  = 0x"); uart_hex(unicam_get_ibls()); uart_puts("\n");
        uart_puts("  IHWIN = 0x"); uart_hex(unicam_get_ihwin());uart_puts("\n");
        uart_puts("  IVWIN = 0x"); uart_hex(unicam_get_ivwin());uart_puts("\n");
        uart_puts("UNICAM_END\n");

        uart_puts("IMX708_BEGIN\n");
        static const uint16_t kReadback[] = {
            0x0100, 0x0101, 0x0114,
            0x0202, 0x0203, 0x0204, 0x0205, 0x020E, 0x020F,
            0x0310,
            0x0340, 0x0341, 0x0342, 0x0343,
            0x034C, 0x034D, 0x034E, 0x034F,
            0x0900,
        };
        for (uint32_t i = 0; i < sizeof(kReadback)/sizeof(uint16_t); i++) {
            uint16_t r = kReadback[i];
            uint8_t v = imx708_read(r);
            uart_puts("  reg=0x"); uart_hex((uint32_t)r);
            uart_puts(" val=0x"); uart_hex((uint32_t)v); uart_puts("\n");
        }
        uart_puts("IMX708_END\n");
    }

    /* Steady-state loop. Two-buffer rotation with continuous DMA, two
     * FSIs per LIP cycle (one full sensor frame per buffer):
     *
     *   active_buf  = the buffer DMA is currently writing into.
     *   prev_buf    = the buffer that holds the just-completed frame.
     *
     * Each iteration:
     *   (1) sync any in-flight debayer from the previous iteration BEFORE
     *       we re-stage that buffer as the next DMA target — ensures
     *       cores 1-3 are done reading it. In steady state ~0 ms because
     *       debayer (~32 ms) finishes well inside the 117 ms wait.
     *   (2) pre-stage IBSA0 = OTHER buffer; takes effect at next LIP.
     *   (3) wait for two FSIs (= one full sensor period). The FIRST FSI
     *       triggers LIP (commits next_buf as the new DMA target); the
     *       SECOND FSI confirms one image frame has been written.
     *   (4) swap state: prev_buf = active_buf; active_buf = next_buf.
     *   (5) invalidate the prev_buf cache lines so debayer reads fresh
     *       SDRAM data (DMA bypasses CPU caches).
     *   (6) dispatch debayer of prev_buf → FB, async on cores 1-3.
     *
     * The debayer overlaps with the next wait_FS, hiding 32 ms inside
     * the 117 ms sensor frame period. Per-frame total ≈ 117 ms = 8.55 fps. */
    uint8_t* active_buf        = raw_a;
    bool     debayer_in_flight = false;
    uint32_t cycle             = 0u;
    while (1) {
        uint64_t t0 = cnt_now();

        /* Step 1: sync with previous iteration's debayer. After this,
         * cores 1-3 are no longer reading prev_buf-of-prev-iter (which
         * we're about to stage as the next DMA target). */
        if (debayer_in_flight) {
            mc_wait_debayer_done();
        }
        uint64_t t1 = cnt_now();

        /* Step 2: pre-stage IBSA0 = next_buf. The register write doesn't
         * affect the active DMA shadow until the LIP that fires inside
         * wait_fs_and_lip below. */
        uint8_t* next_buf = (active_buf == raw_a) ? raw_b : raw_a;
        unicam_stage_dma_buffer(next_buf);

        /* Step 3: wait for two FSIs (one full sensor period). LIP fires
         * at the 1st FSI to commit next_buf; we return after the 2nd
         * FSI when one full image frame is in next_buf. The returned
         * value is IBWP sampled BEFORE the LIP — i.e. the byte count of
         * the just-completed frame inside active_buf. */
        uint32_t ibwp_pre_lip = unicam_wait_fs_and_lip();
        uint64_t t2 = cnt_now();

        /* Step 4: swap state. */
        uint8_t* prev_buf = active_buf;
        active_buf        = next_buf;

        /* One-shot rows_max diagnostic on the first real captured frame. */
        if (cycle == 0u) {
            uint32_t prev_bus = 0xC0000000u | (uint32_t)(uintptr_t)prev_buf;
            uint32_t ibls     = unicam_get_ibls();
            uint32_t bytes_seen = (ibwp_pre_lip > prev_bus) ? (ibwp_pre_lip - prev_bus) : 0u;
            uint32_t rows_max = ibls ? (bytes_seen / ibls) : 0u;
            uart_puts("FRAME rows_max=0x"); uart_hex(rows_max);
            uart_puts(" / 0xA20\n");
            cycle = 1u;
        }

        /* Step 5: invalidate prev_buf cache lines. DMA wrote there
         * bypassing CPU caches, so without this debayer reads stale data. */
        dcache_invalidate_range(prev_buf, FRAME_BYTES);
        uint64_t t3 = cnt_now();

        /* Step 6: dispatch debayer of prev_buf, async on cores 1-3. */
        mc_dispatch_debayer_async(prev_buf, fb, fb_stride_px);
        debayer_in_flight = true;
        uint64_t t4 = cnt_now();

        uint32_t ms_sync     = cnt_to_ms(t1 - t0);
        uint32_t ms_wait     = cnt_to_ms(t2 - t1);   /* incl. stage_buf */
        uint32_t ms_inval    = cnt_to_ms(t3 - t2);
        uint32_t ms_dispatch = cnt_to_ms(t4 - t3);
        uint32_t ms_total    = cnt_to_ms(t4 - t0);
        uint32_t fps_x100    = ms_total ? (100000u / ms_total) : 0u;

        uart_puts("TIMING sync=");  uart_dec(ms_sync);
        uart_puts(" wait=");        uart_dec(ms_wait);
        uart_puts(" inval=");       uart_dec(ms_inval);
        uart_puts(" disp=");        uart_dec(ms_dispatch);
        uart_puts(" total=");       uart_dec(ms_total);
        uart_puts(" ms  fps=");     uart_dec(fps_x100 / 100u);
        uart_putc('.');
        uint32_t frac = fps_x100 % 100u;
        if (frac < 10u) uart_putc('0');
        uart_dec(frac);
        uart_putc('\n');
    }
}
