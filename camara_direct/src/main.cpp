/* camara_direct V160 — all-4-cores debayer with band-while-wait on core 0.
 *
 * V159 binned mode reached ~40 fps with 3-core debayer (cores 1-3, 360
 * rows each) and core 0 dedicated to FSI polling. V160 adds core 0 to
 * the debayer pool: it now runs band 0 (rows 0..269) interleaved with
 * its FSI polls inside wait_fs_and_lip_with_band(). All 4 cores → 270
 * rows each → ~17 ms debayer, hidden inside the 15-20 ms FSI wait
 * window. Target: ~50 fps.
 *
 * The interleaving is row-grained: between every two ISTA polls,
 * core 0 processes one full FB row (~63 µs) of band 0. Polling
 * latency for FSI detection stays well under the 15 ms FS-to-FS
 * interval, so frame timing is unaffected.
 *
 * — V159 below (preserved for context) —
 *
 * V159 — binned 1536×864 mode for higher fps.
 *
 * Path chosen vs V158 (4608×2592 full mode, 7.4 fps):
 *   V158 hit the conservation law `cycle = frame_period + memory_contention`
 *   and stayed at 7.4 fps no matter how the dispatch was reordered, because
 *   the sensor at 4608×2592 / 2-lane / FLL=0x0A5A has a 117 ms physical
 *   frame period AND the 14.93 MB raw buffer + 8.3 MB FB writes saturate
 *   the LPDDR2 bus during debayer.
 *   V159 switches the IMX708 to its native binned mode: 3072×1728 analog
 *   readout window centered in the 4608×2592 array, 2×2 binned to 1536×864
 *   output. This both shortens the frame period (FLL=0x046D=1133 lines
 *   instead of 0x0A5A=2650 → ~24 fps cap) and shrinks the raw buffer by
 *   9× (1.58 MB instead of 14.93 MB), drastically reducing memory pressure
 *   on the debayer + display pipeline.
 *
 * V158's continuous-DMA architecture is preserved verbatim: two raw
 * buffers (raw_a, raw_b), pre-stage IBSA0 + LIP at FS1 of every real
 * frame, wait FS2 to confirm completion, debayer the just-completed
 * buffer async on cores 1-3 while DMA fills the new buffer.
 *
 * IBSA0 rotation (NOT IBSA1/IBEA1 hardware ping-pong) is preserved from
 * V158. The libcamera runtime dumps show bcm2835-unicam.c rewrites IBSA0
 * between frames; IBSA1/IBEA1 are never programmed in libcamera and most
 * plausibly belong to a second DMA channel for embedded data. The proven
 * libcamera pattern is followed.
 *
 * Memory layout (binned mode reduces buffer footprint ~9×):
 *   RAW_A 0x01000000 .. 0x01195000   (1.58 MB DMA target, ping)
 *   RAW_B 0x02000000 .. 0x02195000   (1.58 MB DMA target, pong)
 *
 * Debayer geometry switches from 6/5 downscale (V156-V158) to 5/4 upscale:
 * 1536×864 sensor → 1920×1080 display. Each binned 2×2 Bayer block maps
 * to ~2.5 output pixels (nearest-neighbor for now; bilinear is a future
 * improvement to smooth the upscale).
 *
 * Sensor: Pi Camera v3 binned readout, Bayer is BGGR after the 0x0101=0x03
 * H+V flip Linux applies. ISP constants (BLC, WB, CCM, gamma) carry over
 * unchanged from V158 — they're tied to the IMX708's color response, not
 * the readout mode:
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

#define SENSOR_W   1536u             /* V159 binned: 1536x864 from 3072x1728 crop, 2x2 binned */
#define SENSOR_H   864u
#define RAW_STRIDE 1920u             /* 1536 px * 10 bit / 8 */
#define FRAME_BYTES (RAW_STRIDE * SENSOR_H)   /* 1,658,880 = 0x195000 */

/* ── Multi-core task pool (V160: all 4 cores debayer) ──────────────────────
 * Cores 1, 2, 3 run debayer bands when woken from WFE. Core 0 *also* runs
 * a band — but inline, interleaved with its FSI polling inside the new
 * wait_fs_and_lip_with_band(). Per-frame split with 4 bands of 270 rows
 * (FB_H / 4 exact):
 *
 *   band 0 (rows 0..269)    → core 0 (during its wait_fs polling loop)
 *   band 1 (rows 270..539)  → core 1 (WFE-driven)
 *   band 2 (rows 540..809)  → core 2 (WFE-driven)
 *   band 3 (rows 810..1079) → core 3 (WFE-driven)
 *
 * Sequencing per frame:
 *   1. core 0's mc_dispatch_debayer_async sets g_task_* and bumps
 *      g_task_epoch with RELEASE + sev. Cores 1-3 wake.
 *   2. cores 1-3 run their bands and RELEASE-increment g_task_done.
 *   3. core 0 enters wait_fs_and_lip_with_band, which polls FSI and runs
 *      its band's rows between polls. The band finishes mid-wait or just
 *      after FS2, depending on which is faster.
 *   4. before the next dispatch, core 0 calls mc_wait_debayer_done to
 *      ensure cores 1-3 finished (they only count up to BAND_COUNT-1=3
 *      since core 0 doesn't increment g_task_done).
 *
 * Per-iter cycle: max(wait_fs(~15-20 ms), debayer_per_core(~17 ms)).
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

#define BAND_COUNT 4u                 /* 4 cores total (0 + 1, 2, 3) */
#define BAND_H (FB_H / BAND_COUNT)    /* 270 rows per core (1080/4 exact) */

static inline void band_for_core(uint32_t core_id,
                                 uint32_t* vy_start, uint32_t* vy_end) {
    /* core_id ∈ {0,1,2,3}; each gets BAND_H consecutive output rows. */
    *vy_start = core_id * BAND_H;
    *vy_end   = (core_id == BAND_COUNT - 1u) ? FB_H : ((core_id + 1u) * BAND_H);
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

    /* Task work loop — cores 1, 2, 3 cover bands 1, 2, 3 (270 rows each).
     * Core 0's band (band 0) is run inline by main inside
     * wait_fs_and_lip_with_band. */
    uint32_t local_epoch = 0;
    uint32_t vy_start, vy_end;
    band_for_core(core_id, &vy_start, &vy_end);
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
    /* Wait for cores 1, 2, 3 (the 3 secondaries) to each increment
     * g_task_done once. Core 0 doesn't increment because its band is
     * run inline by main and doesn't go through the WFE path. So the
     * counter target is BAND_COUNT - 1 = 3, not BAND_COUNT.
     *
     * Polling (not WFE) so we don't deadlock if a secondary hangs.
     * Healthy 4-core debayer is ~17 ms; timeout is generous (~1 s). */
    const uint32_t kSecondariesTarget = BAND_COUNT - 1u;
    const uint32_t timeout_iters = 200000000u;
    uint32_t spins = 0;
    while (__atomic_load_n(&g_task_done, __ATOMIC_ACQUIRE) < kSecondariesTarget) {
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
/* IMX708 tuned gamma curve (linux_extract/tuning/imx708.json → rpi.contrast).
 * 51 control points, 16-bit input → 16-bit output, piecewise linear.
 * Built into k_gamma[256] (8-bit→8-bit) at boot in debayer_init_tables(). */
static const uint16_t k_gamma_pts[51][2] = {
    {    0,     0}, {  512,  2518}, { 1024,  5033}, { 1536,  7175},
    { 2048,  9309}, { 2560, 10814}, { 3072, 12312}, { 3584, 13773},
    { 4096, 15225}, { 4608, 16566}, { 5120, 17899}, { 5632, 19221},
    { 6144, 20534}, { 6656, 21684}, { 7168, 22826}, { 7680, 24024},
    { 8192, 25212}, { 9216, 27251}, {10240, 29167}, {11264, 30947},
    {12288, 32696}, {13312, 34309}, {14336, 35849}, {15360, 37194},
    {16384, 38445}, {17408, 39598}, {18432, 40732}, {19456, 41717},
    {20480, 42687}, {22528, 44343}, {24576, 45871}, {26624, 47222},
    {28672, 48441}, {30720, 49460}, {32768, 50470}, {34816, 51476},
    {36864, 52480}, {38912, 53382}, {40960, 54294}, {43008, 55155},
    {45056, 56035}, {47104, 56920}, {49152, 57824}, {51200, 58737},
    {53248, 59666}, {55296, 60604}, {57344, 61558}, {59392, 62529},
    {61440, 63516}, {63488, 64519}, {65535, 65535},
};
static uint8_t k_gamma[256];

static void build_gamma_lut() {
    uint32_t j = 0;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t in16 = i * 257u;   /* 0→0, 255→65535 */
        while (j + 1u < 50u && k_gamma_pts[j + 1u][0] < in16) j++;
        uint32_t x0 = k_gamma_pts[j][0],     y0 = k_gamma_pts[j][1];
        uint32_t x1 = k_gamma_pts[j + 1][0], y1 = k_gamma_pts[j + 1][1];
        uint32_t out16 = (x1 == x0) ? y0
                       : y0 + ((y1 - y0) * (in16 - x0)) / (x1 - x0);
        uint32_t out8  = (out16 + 128u) >> 8;
        if (out8 > 255u) out8 = 255u;
        k_gamma[i] = (uint8_t)out8;
    }
}

/* ── Auto-exposure ─────────────────────────────────────────────────────────
 * Closed-loop CIT control on core 0. Runs between mc_dispatch_debayer_async
 * (cores 1-3 busy) and the next iteration's wait_fs — pure slack time.
 *
 * Sample: 32×32 grid of raw bytes from prev_buf (1024 points). Each byte is
 * the high 8 bits of the 10-bit packed pixel — channel-mixed but a fine
 * luminance proxy after BLC removal.
 *
 * Control: pure-P clamp, target=AE_TARGET, k=1/8, |delta| <= 32 lines/update.
 * Update every AE_PERIOD frames (~12 Hz at 47 fps) to avoid oscillation and
 * I2C overhead in the inner loop. */
#define AE_TARGET    100u   /* post-BLC mean target (mid-warm grey) */
#define AE_BLC          16u  /* BLACK_LVL>>2 in 8-bit space */
#define AE_PERIOD       4u
#define AE_CIT_MIN     16u
#define AE_CIT_MAX   1110u   /* FLL=1133, headroom 22 lines */

static uint16_t s_ae_cit       = 1131u;   /* matches imx708_regs.h initial 0x046B */
static uint32_t s_ae_frame     = 0u;

static uint32_t ae_sample_mean(const uint8_t* raw) {
    uint32_t sum = 0;
    for (uint32_t y = 8u; y < 864u; y += 27u) {           /* 32 rows */
        const uint8_t* row = raw + y * 1920u;             /* RAW_STRIDE */
        for (uint32_t x = 16u; x < 1536u; x += 48u) {     /* 32 cols */
            uint32_t off = (x >> 2) * 5u + (x & 3u);
            sum += row[off];
        }
    }
    return sum >> 10;   /* 1024 samples → mean */
}

static void ae_step(const uint8_t* prev_buf) {
    s_ae_frame++;
    if ((s_ae_frame & (AE_PERIOD - 1u)) != 0u) return;

    uint32_t mean_raw = ae_sample_mean(prev_buf);
    int32_t  mean8    = (int32_t)mean_raw - (int32_t)AE_BLC;
    if (mean8 < 0) mean8 = 0;

    int32_t error = (int32_t)AE_TARGET - mean8;
    int32_t delta = error >> 3;          /* k_p = 1/8 */
    if (delta >  32) delta =  32;        /* slew cap */
    if (delta < -32) delta = -32;

    int32_t cit = (int32_t)s_ae_cit + delta;
    if (cit < (int32_t)AE_CIT_MIN) cit = AE_CIT_MIN;
    if (cit > (int32_t)AE_CIT_MAX) cit = AE_CIT_MAX;
    if ((uint16_t)cit == s_ae_cit) return;

    s_ae_cit = (uint16_t)cit;
    /* Group-hold so high+low bytes latch atomically at next FS. */
    imx708_write(0x0104, 0x01);
    imx708_write(0x0202, (uint8_t)(s_ae_cit >> 8));
    imx708_write(0x0203, (uint8_t)(s_ae_cit & 0xFF));
    imx708_write(0x0104, 0x00);
}

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

/* ── Debayer 1536x864 BGGR → 1920x1080 ─────────────────────────────────────
 *
 * V159 binned: sensor → display ratio is 5/4 in both axes (UPSCALE, not the
 * V158-and-earlier downscale 6/5). Sensor is 1536×864 = 768×432 Bayer 2×2
 * blocks; display is 1920×1080. For output pixel (vx,vy):
 *   blk_x = (vx * 768) / 1920 = (vx * 2) / 5     // vx ∈ [0..1919] → blk_x ∈ [0..767]
 *   blk_y = (vy * 432) / 1080 = (vy * 2) / 5     // vy ∈ [0..1079] → blk_y ∈ [0..431]
 *
 * Each 2×2 block of binned sensor pixels maps to a 5/4 × 5/4 output region
 * (~2.5 outputs per block axis = blocky nearest-neighbor look). Bilinear
 * smoothing is a future improvement; for now nearest-neighbor with the
 * existing NEON ISP pipeline is correct and fast enough.
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
    build_gamma_lut();
    for (uint32_t vy = 0; vy < FB_H; vy++) {
        uint32_t blk_y = (vy * 2u) / 5u;             /* upscale 5/4 in block coords */
        g_row_off[vy] = (blk_y * 2u) * RAW_STRIDE;
    }
    for (uint32_t vx = 0; vx < FB_W; vx++) {
        uint32_t blk_x   = (vx * 2u) / 5u;
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
 * accumulates into int32 with no intermediate truncation.
 *
 * V160: extracted as a per-row function so wait_fs_and_lip_with_band can
 * call it once per loop iteration and interleave row work with FSI polling
 * on core 0. */
static void debayer_one_row(uint32_t vy,
                            const uint8_t* raw, volatile uint32_t* fb,
                            uint32_t fb_stride_px) {
    const uint16x4_t v_ped   = vdup_n_u16((uint8_t)(BLACK_LVL >> 2));
    const int32x4_t  v_zero  = vdupq_n_s32(0);
    const int32x4_t  v_max   = vdupq_n_s32(255);
    const int32x4_t  v_round = vdupq_n_s32(512);

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

            fb_row[vx + 0] = 0xFF000000u | (k_gamma[r0i] << 16) | (k_gamma[g0i] << 8) | k_gamma[b0i];
            fb_row[vx + 1] = 0xFF000000u | (k_gamma[r1i] << 16) | (k_gamma[g1i] << 8) | k_gamma[b1i];
            fb_row[vx + 2] = 0xFF000000u | (k_gamma[r2i] << 16) | (k_gamma[g2i] << 8) | k_gamma[b2i];
            fb_row[vx + 3] = 0xFF000000u | (k_gamma[r3i] << 16) | (k_gamma[g3i] << 8) | k_gamma[b3i];
    }
}

/* Wrapper used by secondary cores (cores 1, 2, 3) to process a band of
 * consecutive output rows. Core 0 doesn't go through this — it interleaves
 * row work with FSI polling inside wait_fs_and_lip_with_band. */
static void debayer_band(uint32_t vy_start, uint32_t vy_end,
                         const uint8_t* raw, volatile uint32_t* fb,
                         uint32_t fb_stride_px) {
    for (uint32_t vy = vy_start; vy < vy_end; vy++) {
        debayer_one_row(vy, raw, fb, fb_stride_px);
    }
}

/* Core 0's wait + band-work-during-wait loop. Implements the V154 2-FSI
 * protocol via the unicam.h primitives, but interleaves debayer rows
 * between FSI polls so core 0 contributes its band of work concurrent
 * with the wait. Returns the IBWP value sampled BEFORE the LIP at FS1
 * (== bytes of the just-completed frame in the buffer that was active
 * before this LIP). If FS2 fires before band finishes, the remaining
 * rows are drained synchronously before returning. */
static uint32_t wait_fs_and_lip_with_band(
        uint32_t band_start, uint32_t band_end,
        const uint8_t* raw, volatile uint32_t* fb, uint32_t fb_stride_px) {
    unicam_arm_for_wait();

    int      fs_count    = 0;
    uint32_t ibwp_pre_lip = 0u;
    uint32_t cur_row      = band_start;
    const uint32_t kMaxIters = 20000000u;

    for (uint32_t i = 0; i < kMaxIters; i++) {
        if (unicam_consume_fsi()) {
            fs_count++;
            if (fs_count == 1) {
                ibwp_pre_lip = unicam_get_ibwp();
                unicam_lip_strobe();
            } else {
                /* FS2: drain remaining rows synchronously before returning so
                 * the FB scanout sees a complete frame on this iteration. */
                while (cur_row < band_end) {
                    debayer_one_row(cur_row, raw, fb, fb_stride_px);
                    cur_row++;
                }
                return ibwp_pre_lip;
            }
        }
        if (cur_row < band_end) {
            debayer_one_row(cur_row, raw, fb, fb_stride_px);
            cur_row++;
        }
    }

    /* Timeout fallback — drain band to keep FB consistent. Caller will
     * see an unexpected return value and probably stutter, but won't tear. */
    while (cur_row < band_end) {
        debayer_one_row(cur_row, raw, fb, fb_stride_px);
        cur_row++;
    }
    return 0u;
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
    uart_puts("camara_direct V160: IMX708 1536x864 binned -> HDMI 1920x1080 (4-core debayer, core 0 band-while-wait)\n");

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
     * state and can be diff'd against linux_capture_linux/. */
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

    /* Steady-state loop. V160 — all 4 cores debayer:
     *
     *   active_buf  = the buffer DMA is currently writing into.
     *   prev_buf    = the buffer that holds the just-completed frame.
     *   g_task_raw  = the buffer cores 1-3 are debayering (set by the
     *                 dispatch at the END of the previous iteration; its
     *                 value at iter K's start equals iter K-1's prev_buf).
     *
     * Each iteration:
     *   (1) pre-stage IBSA0 = OTHER buffer; takes effect at next LIP.
     *   (2) wait_fs_and_lip_with_band: poll for two FSIs (one full sensor
     *       period). At FS1 strobe LIP and commit next_buf as DMA target.
     *       Between polls, run core 0's band of g_task_raw — same buffer
     *       cores 1-3 are working on. At FS2 drain remaining rows of
     *       core 0's band synchronously and return.
     *   (3) swap state: prev_buf = active_buf; active_buf = next_buf.
     *   (4) sync — wait for cores 1, 2, 3 to finish their bands. Healthy:
     *       ~0 ms because the 17 ms 4-core debayer fits inside the 15-20 ms
     *       wait_fs window.
     *   (5) invalidate prev_buf cache lines.
     *   (6) dispatch debayer of prev_buf → FB, async on cores 1-3 for the
     *       NEXT iteration's wait window.
     *
     * Per-frame total: max(wait_fs(~15-20 ms), debayer4(~17 ms)) ≈ ~20 ms
     * → ~50 fps target (vs V159's 38-45 fps with 3-core debayer). */
    uint8_t* active_buf        = raw_a;
    bool     debayer_in_flight = false;
    while (1) {
        uint64_t t0 = cnt_now();

        /* Step 1: pre-stage IBSA0 = next_buf. The register write doesn't
         * affect the active DMA shadow until the LIP that fires inside
         * wait_fs_and_lip below. */
        uint8_t* next_buf = (active_buf == raw_a) ? raw_b : raw_a;
        unicam_stage_dma_buffer(next_buf);

        /* Step 2: wait for two FSIs. If a previous iteration dispatched
         * a debayer, run core 0's band of that same buffer (g_task_raw)
         * during the wait. Otherwise just poll without doing band work. */
        if (debayer_in_flight) {
            uint32_t bs, be;
            band_for_core(0u, &bs, &be);
            (void)wait_fs_and_lip_with_band(
                bs, be, g_task_raw, fb, fb_stride_px);
        } else {
            (void)unicam_wait_fs_and_lip();
        }
        uint64_t t1 = cnt_now();

        /* Step 3: swap state. */
        uint8_t* prev_buf = active_buf;
        active_buf        = next_buf;

        /* Step 4: sync with cores 1-3 of the previous iteration's debayer.
         * Healthy: ~0 ms because all three secondaries finish their
         * 17 ms bands inside the 15-20 ms wait above. */
        if (debayer_in_flight) {
            mc_wait_debayer_done();
        }
        uint64_t t2 = cnt_now();

        /* Step 5: invalidate prev_buf cache lines. DMA wrote there
         * bypassing CPU caches, so without this debayer reads stale data. */
        dcache_invalidate_range(prev_buf, FRAME_BYTES);
        uint64_t t3 = cnt_now();

        /* Step 6: dispatch debayer of prev_buf, async on cores 1-3. */
        mc_dispatch_debayer_async(prev_buf, fb, fb_stride_px);
        debayer_in_flight = true;
        uint64_t t4 = cnt_now();

        /* Step 7: AE — measure prev_buf and (every AE_PERIOD frames)
         * push a new CIT to the sensor. Cores 1-3 are reading prev_buf
         * for debayer; ae_sample_mean is read-only so no conflict. */
        ae_step(prev_buf);

        uint32_t ms_wait     = cnt_to_ms(t1 - t0);   /* incl. stage_buf */
        uint32_t ms_sync     = cnt_to_ms(t2 - t1);
        uint32_t ms_inval    = cnt_to_ms(t3 - t2);
        uint32_t ms_dispatch = cnt_to_ms(t4 - t3);
        uint32_t ms_total    = cnt_to_ms(t4 - t0);
        uint32_t fps_x100    = ms_total ? (100000u / ms_total) : 0u;

        uart_puts("TIMING wait=");  uart_dec(ms_wait);
        uart_puts(" sync=");        uart_dec(ms_sync);
        uart_puts(" inval=");       uart_dec(ms_inval);
        uart_puts(" disp=");        uart_dec(ms_dispatch);
        uart_puts(" total=");       uart_dec(ms_total);
        uart_puts(" ms  fps=");     uart_dec(fps_x100 / 100u);
        uart_putc('.');
        uint32_t frac = fps_x100 % 100u;
        if (frac < 10u) uart_putc('0');
        uart_dec(frac);
        uart_puts("  ae_cit=");     uart_dec(s_ae_cit);
        uart_putc('\n');
    }
}
