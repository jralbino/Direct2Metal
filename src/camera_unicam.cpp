/* File: src/camera_unicam.cpp
 * V111 — Complete BCM2837 Unicam1 CSI-2 Driver + Hardware-Faithful Simulator
 *
 * ── CRITICAL BUGS FIXED vs V75 ──────────────────────────────────────────────
 *   [A] IDI0 at 0x108 = 0x2A (RAW8, VC=0) — was COMPLETELY MISSING in V75!
 *   [B] IPIPE at 0x10C = 0x00 (RAW8 passthrough) — was COMPLETELY MISSING in V75!
 *   [C] IBEA0 at 0x114 = IBSA0+frame_size — was COMPLETELY MISSING in V75!
 *   [D] IBLS at 0x118 = FRAME_W — was COMPLETELY MISSING in V75!
 *   [E] MISC at 0x400 = FL0|FL1 — was MISSING in V75!
 *   [F] CLKGATE at 0x3F802004 (CSI1!) — was MISSING in V75! (V106: re-corrected from 0x3F802000)
 *   [G] ANA power-up before CPR — was MISSING in V75 (regression from V72)!
 *   [H] CLT/DLT timing registers — were MISSING in V75!
 *   [I] Removed incorrect U_CTRL_LSM = BIT(14) — BIT(14) is OET field, not LSM!
 *   [J] CMP0 at 0x02C = 0x80000301 — secondary Frame End detection path
 *   [K] PRI at 0x00C = 0xE85 — AXI priority (Linux sets this)
 *   [L] IHWIN/IVWIN cleared — crop disabled
 *   [M] STA/ISTA cleared before enabling interrupts
 *   [N] DAT1 = 0x00 for 1-lane (disabled, not 0x02)
 *
 * ── V79 CHANGES ──────────────────────────────────────────────────────────────
 *   [O] ANA = 0xFF0 — confirmed by Linux bcm2835-unicam.c uses 0x770; we use 0xFF0
 *       for maximum D-PHY bias. V80 confirmed VPU leaves ANA=0x777 (not calibrated).
 *   [P] Simulator: CM_CAM1CTL added as precondition.
 *   [Q] Simulator: ICTL LIP bit now self-clears on readback.
 *   [R] Simulator: STA register models FS/FE.
 *   [S] Simulator: CLKhi/D0hi lane counters change after sensor stream_on.
 *   [T] Simulator: ANA CTATADJ/PTATADJ < 0xF flagged as failure.
 *
 * ── V80 CHANGES ──────────────────────────────────────────────────────────────
 *   [U] Print VPU-left ANA value BEFORE our write.
 *   [V] Conditional ANA write: preserve VPU calibration if present, else 0xFF0.
 *   [W] DLT/CLT settle time 0x0602→0x1502 (settle=21, 188ns > MIPI spec 91.7ns).
 *   [X] STA/ISTA/IBWP periodic diagnostic during capture polling.
 *   [Y] IMX708 0x0114 readback to verify 1-lane override.
 *
 * ── V93 CHANGES ──────────────────────────────────────────────────────────────
 *   [AL] *** ROOT CAUSE FIX: V90 accidentally set CPM=CCP2, breaking CSI-2 decode ***
 *
 *   CTRL register field layout (from vc4-regs-unicam.h, NOT what V90 assumed):
 *     BIT(0) = CPE  BIT(1) = MEM  BIT(2) = CPR
 *     BIT(3) = CPM_MASK (0=CSI-2, 1=CCP2)  BIT(4) = SOE  ...
 *   "UNICAM_DAT_LANES_SHIFT=3" DOES NOT EXIST in bcm2835-unicam.c.
 *   Data lanes are enabled by writing 0x1D to DAT0/DAT1 registers (offset 0x018/0x01C).
 *
 *   V90 computed lane_bits = BIT(3)|BIT(4) = 0x18 and OR'd into CTRL:
 *   → BIT(3)=1 = CPM = 1 = CCP2 mode → Unicam tries to decode CCP2 serial protocol
 *   → sensor sends MIPI CSI-2 → every packet is garbage to the CCP2 decoder
 *   → STA=0 GUARANTEED (explains V90, V91, V92 persistent STA=0 despite all fixes)
 *   Fix: remove lane_bits — CTRL = U_CTRL_BASE = 0x080F02 (CPM=0=CSI-2)
 *
 *   [AM] *** FIX: CLKGATE value is NOT a lane count — it uses a shift+OR algorithm ***
 *
 *   Linux bcm2835-unicam.c clk_write() ALWAYS ORs 0x5A000000 (same as CM password):
 *     static inline void clk_write(struct unicam_device *dev, u32 val) {
 *         writel(val | 0x5a000000, dev->clk_gate_base);
 *     }
 *   And val is computed by: val=1; for each lane: val = (val<<2)|1
 *     1-lane: val=5  → write 0x5A000005
 *     2-lane: val=21 → write 0x5A000015
 *   Previous versions: V83 wrote 0x5A000005 for 1-lane (accidentally correct!), then
 *   "fixed" it to raw 0x05 (removed password). V88 changed to lane count (0x01/0x02).
 *   Fix: 2-lane → 0x5A000015, 1-lane → 0x5A000005.
 *
 * ── V92 CHANGES ──────────────────────────────────────────────────────────────
 *   [AK] *** FIX (WRONG): CLKGATE address changed 0x3F802004 → 0x3F802000 ***
 *   This was WRONG. V106 discovered via bcm2835-peripherals.dtsi that CSI0=0x3F802000
 *   and CSI1=0x3F802004. We need CSI1 (Unicam1) → 0x3F802004. V106 re-corrected.
 *
 * ── V91 CHANGES ──────────────────────────────────────────────────────────────
 *   [AJ] *** ROOT CAUSE FIX: IDI0=0x2A (RAW8) does NOT match sensor output ***
 *   IMX708 has no RAW8 mode. k_imx708_common sets 0x0112/0x0113=0x0A (RAW10).
 *   Our 0x0112=0x08 override in imx708_init() is ignored — sensor still sends
 *   DT=0x2B (RAW10). Frame-count=52fps at 450Mbps/lane confirms 10-bit pixels
 *   (52 * 1536 * 864 * 10 bits / 2 lanes ≈ 345 Mbps/lane, consistent with 450Mbps).
 *   Unicam IDI0=0x2A packet filter drops EVERY RAW10 packet → STA=0 forever.
 *   Fix: IDI0=0x2B (RAW10) and FRAME_W=1920 (RAW10-packed stride: 1536*10/8=1920).
 *
 * ── V90 CHANGES (SUPERSEDED — INTRODUCED CCP2 BUG, FIXED IN V93) ─────────────
 *   [AI] *** WRONG FIX: Added non-existent "UNICAM_DAT_LANES_SHIFT=3" to CTRL ***
 *   This hypothesis was incorrect. BIT(3) in CTRL is CPM (Camera Port Mode),
 *   not a data lane enable. Setting lane_bits=0x18 put Unicam in CCP2 mode.
 *   V93 reverts this change. V90 also confirmed D1 physically connected (D1hi=57344 HS).
 *
 * ── V89 CHANGES ──────────────────────────────────────────────────────────────
 *   [AH] *** TEST: 2-lane mode — re-examine V73 "silent" conclusion ***
 *   V73 observed D0hi=D1hi=LP-11 constant in 2-lane mode and concluded D1 is
 *   physically broken. BUT V73 had the CPR bug: CLK/DAT0 written before CPR →
 *   CPR wiped them to 0x02 (power-down) → Unicam never asserted 100Ω termination
 *   on D1 → IMX708 sees missing termination on D1 → sensor keeps both lanes in
 *   LP-11 → "SILENT." CPR bug = real cause, not physical D1 failure.
 *
 *   V89 tests 2-lane with all current fixes (CPR fixed, ANA=0x770, settle=6):
 *   - IMX708: 0x0114=0x01 (2-lane, k_imx708_common default)
 *   - DAT1 = 0x1D (100Ω termination on D1 → sensor sees valid 2-lane RX)
 *   - CLKGATE = 0x02 (2 active data lanes per bcm2835-unicam.c convention)
 *
 *   Expected: if D1 is physically connected, sensor now sees proper termination
 *   on both D0 and D1 → transitions to 2-lane HS → STA shows FS/FE.
 *
 * ── V88 CHANGES ──────────────────────────────────────────────────────────────
 *   [AG] *** FIX: CLKGATE = 0x01 (active_data_lanes count), NOT 0x05 (bitmask) ***
 *   Linux bcm2835-unicam.c: writel(dev->active_data_lanes, dev->clkgate_regs)
 *   where active_data_lanes = NUMBER of data lanes (1 or 2), NOT a bitmask.
 *   For 1-lane: write 1. For 2-lane: write 2. Linux writes 2 for IMX708 (2-lane).
 *   We have been writing 0x05 = BIT(0)|BIT(2) (our assumed "CLK gate + D0 gate"
 *   bitmask interpretation). Writing 5 to a register that expects a lane count
 *   (hardware uses bits[1:0] as: 0=off, 1=1lane, 2=2lane) sends an invalid/garbage
 *   value. The decoder clock gate may be left in an undefined state → decoder
 *   never starts → STA=0, IBWP stuck, even though physical MIPI is active.
 *   Fix: write 0x01 (1 active data lane) for our 1-lane configuration.
 *
 * ── V87 CHANGES ──────────────────────────────────────────────────────────────
 *   [AF] *** FIX: flush mbox[] D-cache to DRAM before mbox_call (DC CIVAC) ***
 *   mbox[] lives in Normal-cacheable RAM. With D-cache enabled, writes to mbox[]
 *   stay in L1/L2 D-cache and never reach DRAM until flushed. VideoCore reads from
 *   ARM DRAM physical address → sees stale/zero values → doesn't respond →
 *   mbox[1] stays 0x00000000 → mbox_call() returns false. video_init() works because
 *   it runs before D-cache becomes dirty for the mbox range (or mailbox.cpp clears
 *   its own buffer). mbox_flush_to_vc() issues DC CIVAC on each cache line of the
 *   8-word message, plus DSB SY + ISB, before calling mbox_call.
 *
 * ── V86 CHANGES ──────────────────────────────────────────────────────────────
 *   [AE] *** FIX mailbox format: mbox[4]=0 (req_resp_indicator), not buf_size ***
 *   Linux rpi_firmware_property() always sets req_resp_size field to 0 for requests.
 *   V85 sent mbox[4]=8 → firmware saw "8 bytes of request data" for GET (which only
 *   has 4 bytes: device_id) → firmware returned parse error → mbox_call returned 0.
 *   Also: always print mbox[1] (firmware response code) for diagnostics, regardless
 *   of mbox_call return value.
 *
 * ── V85 CHANGES ──────────────────────────────────────────────────────────────
 *   [AC] *** VPU firmware mailbox: SET_POWER_STATE(Unicam1=0x0d, on+wait) ***
 *   Linux calls pm_runtime_get_sync() → genpd_runtime_resume() → VPU firmware
 *   mailbox SET_POWER_STATE (tag 0x00028001), device 0x0d = RPI_POWER_DOMAIN_UNICAM1.
 *   Without this, the CSI-2 decoder may be power-gated (MMIO accessible, pads
 *   active, but decoder logic frozen) → STA=0 forever despite correct register
 *   sequence. Added GET_POWER_STATE first for diagnostic readback.
 *   [AD] CTRL added to periodic diagnostic prints in unicam_capture_frame().
 *
 * ── V84 CHANGES ──────────────────────────────────────────────────────────────
 *   [AB] *** ANA reverted to Linux exact: 0x774→0x770 (CTATADJ=PTAT=7) ***
 *   V79 changed ANA to 0xFF0 (CTAT=F, PTAT=F) "for maximum bias" — wrong.
 *   Linux uses 0x770 on the same BCM2837+IMX708 hardware. CTAT/PTAT=F may give
 *   wrong termination impedance → reflections → SYNC byte corrupted → STA=0.
 *   V76-V78 had correct 0x770 but also CPR bug — STA=0 was blamed on wrong cause.
 *   With CPR fixed (V81), settle=6 (V82), and ANA=0x770 (V84), all Linux values
 *   are now matched. Simulator ANA check updated (accept any non-zero CTAT/PTAT).
 *
 * ── V83 CHANGES ──────────────────────────────────────────────────────────────
 *   [AA] *** FIX: CLKGATE written without CM_PASSWD (0x5A000000) ***
 *   CLKGATE at 0x3F802000 is a CSI1 peripheral register, not a CM register.
 *   Previous code wrote CM_PASSWD|0x05 = 0x5A000005, setting spurious bits
 *   [31:8] that may starve the CSI-2 decoder of its clock → STA=0.
 *   Linux writes raw 0x05. CLKGATE readback added for diagnostic.
 *
 * ── V82 CHANGES ──────────────────────────────────────────────────────────────
 *   [Z2] DLT/CLT: 0x1502 → 0x0602 (settle=6, 60ns = Linux exact value).
 *   settle=21 (210ns) exceeded HS preamble (~154ns at 1+ Gbps) → missed SYNC.
 *
 * ── V81 CHANGES ──────────────────────────────────────────────────────────────
 *   [Z] *** ROOT CAUSE FIX: CLK/DAT0 lane config moved to AFTER CPR pulse! ***
 *
 *   In V80 (and all prior versions), CLK and DAT0 lane registers were configured
 *   BEFORE the CPR (Clock Pipe Reset) pulse. The CPR reset wipes the D-PHY
 *   frontend lane registers back to their power-on default (VPU boot state = 0x02
 *   = CLPD/DLPD = clock/data lane powered DOWN). This means after CPR, the
 *   CLK lane was in power-down state — no MIPI clock recovery possible → STA=0.
 *
 *   Linux bcm2835-unicam.c unicam_start_rx() order (verified from rpi-6.6.y):
 *     [1] CTRL = MEM
 *     [2] ANA power-up (with 1-2ms delay)
 *     [3] CPR pulse (set then clear BIT(2))
 *     [4] CTRL = CTRL_BASE (MEM|PFT|OET = 0x080F02)
 *     [5] PRI, IHWIN, IVWIN
 *     [6] ICTL = FSIE|FEIE|IBOB; clear STA, ISTA
 *     [7] CLT, DLT          ← AFTER CPR!
 *     [8] CMP0
 *     [9] CLK, DAT0, DAT1   ← AFTER CPR! (was step 3 in V80 = BUG)
 *    [10] IBLS, IBSA0, IBEA0
 *    [11] IPIPE, IDI0
 *    [12] MISC |= FL0|FL1
 *    [13] CTRL |= CPE
 *    [14] MISC |= FL0|FL1 (Linux re-asserts after CPE)
 *    [15] ICTL |= LIP
 *
 *   Also: ANA delay increased from 200K NOPs (~0.4ms) to 1M NOPs (~1ms) to match
 *   Linux's usleep_range(1000, 2000) mandatory DDL lock wait.
 *   Simulator: updated to validate CLK/DAT0 configured after CPR, not before.
 *
 * ── V105 CHANGES ──────────────────────────────────────────────────────────────
 *   [AN] IBSA0/IBEA0 bus address: 0x40000000 → 0xC0000000 (VideoCore bus alias).
 *   Pi OS IBWP=0xCBD95000 confirms VC uses 0xC0000000|phys, not ARM L2-bypass.
 *
 * ── V106 CHANGES ──────────────────────────────────────────────────────────────
 *   [AO] *** FIX: CLKGATE address 0x3F802000 → 0x3F802004 (CSI1, not CSI0) ***
 *   bcm2835-peripherals.dtsi: csi0=0x7e802000, csi1=0x7e802004.
 *   V92 wrongly "corrected" to 0x3F802000 (CSI0). We need CSI1 → 0x3F802004.
 *   HW confirmed: readback=0x15 (value retained at correct address).
 *
 * ── V107 CHANGES ──────────────────────────────────────────────────────────────
 *   [AP] CLKGATE write moved from Step 0B (before CPR) to Step 14B (after CPE).
 *   Linux bcm2835-unicam.c clk_write() is called AFTER CPE in unicam_start_rx().
 *   Order: CPR → registers → CPE → CLKGATE → MISC → LIP.
 *
 * ── V108 CHANGES ──────────────────────────────────────────────────────────────
 *   [AQ] *** ROOT CAUSE FIX: Replace SET_POWER_STATE with SET_DOMAIN_STATE + SET_CLOCK_RATE ***
 *   Linux bcm2835-unicam.c uses pm_runtime → raspberrypi-genpd → SET_DOMAIN_STATE
 *   (tag 0x00038030), NOT the old SET_POWER_STATE (tag 0x00028001).
 *   domain=14 = RPI_POWER_DOMAIN_UNICAM1 (DT index 13 + 1).
 *   SET_POWER_STATE always returned state=0x02 (wrong tag namespace).
 *   Also: SET_CLOCK_RATE(clock_id=4, rate=250MHz) for CORE clock — Linux DT
 *   specifies firmware_clocks = <4> for bcm2835-unicam node.
 *
 * ── V109 CHANGES ──────────────────────────────────────────────────────────────
 *   [AR] *** FIX: Remove CPR-post-stream diagnostic that destroyed FE detection ***
 *   V108 HW results: STA>0 for first time (SET_DOMAIN_STATE was root cause!).
 *   STA_accum=0xD001 (FS + PI0) BEFORE CPR-post-stream. After CPR: PI0 gone,
 *   FEI never fires. CPR mid-reception resets D-PHY → FE lost.
 *   V109: no CPR in capture. Clear ISTA/STA before capture. Fast-poll for early FEI/PI0.
 *
 * ── SIMULATOR (SIMULATION mode) ─────────────────────────────────────────────
 *   Register-level emulator validates exact Linux driver sequence.
 *   If ALL preconditions met → ISTA_FEI synthesized → capture succeeds.
 *   If ANY step wrong → UART prints specific failure message.
 *
 * ── CONFIRMED REGISTER MAP (vc4-regs-unicam.h, rpi-4.19.y) ─────────────────
 *   CTRL=0x000, STA=0x004, ANA=0x008, PRI=0x00C, CLK=0x010, CLT=0x014
 *   DAT0=0x018, DAT1=0x01C, DAT2=0x020, DAT3=0x024, DLT=0x028, CMP0=0x02C
 *   ICTL=0x100, ISTA=0x104, IDI0=0x108, IPIPE=0x10C
 *   IBSA0=0x110, IBEA0=0x114, IBLS=0x118, IBWP=0x11C
 *   IHWIN=0x120, IVWIN=0x128, MISC=0x400
 */
#include "hardware_sim.h"
#include <stdint.h>

extern void uart_puts(const char* s);
extern void uart_dec(int n);
extern void uart_hex(uint32_t n);
extern void watchdog_kick();

/* VPU firmware mailbox (defined in mailbox.cpp / kernel.cpp) */
extern volatile uint32_t mbox[36];
extern int mbox_call(unsigned char ch);

/* Flush mbox[] buffer to Point of Coherency before sending to VPU.
 *
 * With D-cache enabled (MMU + Normal-cacheable RAM), writes to mbox[] stay
 * in the L1/L2 D-cache and never reach DRAM unless explicitly flushed.
 * The VideoCore DMA reads from the ARM physical DRAM address — it sees
 * the DRAM contents, NOT the cached ARM view. Without a flush, the VPU
 * reads stale/zeroed DRAM → ignores the message → mbox[1] stays 0x00000000
 * → mbox_call() returns false ("FAILED").
 *
 * DC CIVAC: Clean and Invalidate by VA to Point of Coherency.
 *   Clean  = write dirty cache lines to DRAM (VPU can now see our message).
 *   Invalidate = mark line invalid (next read after VPU response fetches
 *                fresh DRAM instead of stale cached value).
 * DSB SY: Data Synchronization Barrier — ensures all CIVAC ops complete.
 * ISB:    Instruction Synchronization Barrier — prevents pipeline reorder.
 *
 * `words` = number of uint32_t words in the message (usually 8 for single-tag).
 */
static void mbox_flush_to_vc(unsigned int words) {
    unsigned long addr = (unsigned long)(void*)mbox;
    unsigned long end  = addr + words * 4u;
    /* BCM2837 Cortex-A53: cache line = 64 bytes */
    for (unsigned long a = addr & ~63UL; a < end; a += 64)
        asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb"    ::: "memory");
}

/* ─── Hardware Base Addresses ─────────────────────────────────────────────── */
#define UNICAM1_BASE        0x3F801000UL
#define UNICAM1_CLKGATE     ((volatile uint32_t*)0x3F802004UL)  /* V106: CSI1! (V92 wrongly changed to 0x3F802000=CSI0) */
#define CM_PASSWD           0x5A000000u

/* CM_CAM1CTL/DIV: BCM2837 Unicam1 digital backend clock (= BCM2835_CLOCK_CAM1)
 * Linux driver calls clk_prepare_enable(dev->clock) for this clock.
 * Without it, the CSI-2 protocol decoder is frozen: STA=0, IBWP never moves.
 * Target: 100 MHz = PLLD(500MHz) / DIVI=5.
 *
 * BCM2835 CM register map (confirmed against CM_GP0CTL=0x070, CM_GP2CTL=0x080):
 *   offset 0x040 = CM_CAM0CTL
 *   offset 0x044 = CM_CAM0DIV
 *   offset 0x048 = CM_CAM1CTL  ← correct address
 *   offset 0x04C = CM_CAM1DIV
 *   offset 0x058 = CM_DSI0ECTL ← WRONG (what was used before, configures DSI0 display clock!)
 */
#define CM_CAM1CTL  ((volatile uint32_t*)0x3F101048UL)   /* offset 0x048 from CM base */
#define CM_CAM1DIV  ((volatile uint32_t*)0x3F10104CUL)   /* offset 0x04C from CM base */

/* ─── CTRL bit definitions ────────────────────────────────────────────────── */
/* (offsets in hardware_sim.h) */

/* ─── Frame geometry ──────────────────────────────────────────────────────── */
/* V91: IMX708 outputs RAW10 (DT=0x2B). CSI-2 packed RAW10: 4 pixels per 5 bytes.
 * Byte stride per line = 1536 pixels * 10 bits / 8 bits/byte = 1920 bytes/line.
 * FRAME_W is the BYTE stride (used for IBLS), not pixel count. */
#define FRAME_W  1920   /* RAW10-packed bytes/line: 1536px * 10bit / 8 = 1920 */
#define FRAME_H   864
#define FRAME_SZ  (FRAME_W * FRAME_H)  /* 1920 * 864 = 1,658,880 bytes */

/* ─── DMA frame buffer ────────────────────────────────────────────────────── */
__attribute__((aligned(64)))
static uint8_t g_raw_frame[FRAME_SZ];

/* ─── Global sim state (defined here, extern in hardware_sim.h) ───────────── */
UnicamSimState g_sim_state = {0};

/* ─── Delays ──────────────────────────────────────────────────────────────── */
static void delay_nop(unsigned int n) {
    for (volatile unsigned int i = 0; i < n; i++) asm volatile("nop");
}

/* ─── DMA cache coherency ────────────────────────────────────────────────── */
/* V110: After DMA completes, frame data is in DRAM but ARM D-cache may hold
 * stale lines (BSS zeros or previous frame). DC CIVAC = Clean + Invalidate:
 *   Clean = writeback dirty lines to DRAM (no-op if DMA already wrote)
 *   Invalidate = mark lines invalid → next ARM read fetches fresh DRAM
 * DC IVAC (invalidate-only) can be UNPREDICTABLE on dirty lines on some
 * implementations. CIVAC is safe regardless of line state. */
static void invalidate_frame_dcache() {
#ifndef SIMULATION
    unsigned long addr = (unsigned long)(void*)g_raw_frame;
    unsigned long end  = addr + FRAME_SZ;
    for (unsigned long a = addr & ~63UL; a < end; a += 64)
        asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
#endif
}

/* V110: Stop DMA after capture — freeze buffer contents.
 * Clear CPE to disable Unicam peripheral. Without this, the sensor at 52fps
 * overwrites the buffer during the ~1100ms debayer, corrupting the frame. */
static void stop_unicam_dma() {
#ifndef SIMULATION
    volatile uint32_t* U1 = (volatile uint32_t*)(uintptr_t)UNICAM1_BASE;
    U1[U_CTRL/4] &= ~U_CTRL_CPE;   /* CPE=0 → peripheral disabled, DMA stops */
    asm volatile("dsb st" ::: "memory");
#endif
}

/* V110: Restart DMA for next capture.
 * Re-enable CPE, re-write CLKGATE, re-assert MISC FL bits, trigger LIP. */
static void restart_unicam_dma() {
#ifndef SIMULATION
    volatile uint32_t* U1 = (volatile uint32_t*)(uintptr_t)UNICAM1_BASE;
    U1[U_CTRL/4] |= U_CTRL_CPE;                    /* CPE=1 */
    *UNICAM1_CLKGATE = 0x5A000015u;                 /* re-assert CLKGATE */
    U1[U_MISC/4] |= (1u << 6) | (1u << 9);         /* MISC FL0|FL1 */
    U1[U_ICTL/4] |= U_ICTL_LIP;                    /* LIP — latch addresses */
    asm volatile("dsb st" ::: "memory");
#endif
}

/* ─── SIMULATION register file and emulation ─────────────────────────────── */
#ifdef SIMULATION

/* Software shadow of Unicam1 MMIO registers (512 × 4 bytes = 2KB) */
static uint32_t s_sim_regs[512];

/* Initialize simulator with BCM2837 power-on / VPU boot state */
static void sim_init_hw_state() {
    /* Zero all simulated registers */
    for (int i = 0; i < 512; i++) s_sim_regs[i] = 0;
    /* VPU leaves ANA=0x777 (D-PHY fully powered down + reset) */
    s_sim_regs[U_ANA/4] = U_ANA_ALL_OFF;
    /* VPU leaves CLK/DAT with CLPD/DLPD=BIT(1) set (power down) */
    s_sim_regs[U_CLK/4]  = 0x02u;
    s_sim_regs[U_DAT0/4] = 0x02u;
    s_sim_regs[U_DAT1/4] = 0x02u;
    s_sim_regs[U_DAT2/4] = 0x02u;
    s_sim_regs[U_DAT3/4] = 0x02u;
}

/* Record first error if not already set */
static void sim_set_error(const char* msg) {
    if (!g_sim_state.error_msg) g_sim_state.error_msg = msg;
}

/* Called on every MMIO write to offset/4 */
static void sim_on_write(uint32_t offset, uint32_t val) {
    /* ISTA is write-to-clear: writing 1s clears the corresponding bits */
    if (offset == U_ISTA) {
        s_sim_regs[U_ISTA/4] &= ~val;
        return;
    }
    s_sim_regs[offset/4] = val;
    g_sim_state.reg_ctrl = s_sim_regs[U_CTRL/4];

    switch (offset) {
    case U_CTRL:
        if (val & U_CTRL_MEM) g_sim_state.mem_bit_set = true;
        /* V93: CPM (BIT(3)) must be 0 for CSI-2 mode. BIT(3)=1 → CCP2 → STA=0. */
        if (val & U_CTRL_CPM)
            sim_set_error("CTRL: CPM=1 (BIT(3) set) — Unicam in CCP2 mode, sensor sends CSI-2 → STA=0. Remove lane bits from CTRL.");
        if (val & U_CTRL_CPR) {
            /* CPR pulse: ANA must be powered ON at this point */
            if (!g_sim_state.ana_powered_up) {
                sim_set_error("CPR pulsed while ANA=0x777 (D-PHY analog OFF) — D-PHY never locks");
                g_sim_state.cpr_before_ana = true;
            } else {
                g_sim_state.ana_before_cpr = true;
            }
        }
        if (val & U_CTRL_CPE) {
            if (!g_sim_state.cpr_pulsed)
                sim_set_error("CPE enabled before CPR pulse — CSI-2 decoder in reset");
            g_sim_state.cpe_enabled = true;
        }
        /* Check PFT field: bits[15:8] */
        if (((val >> 8) & 0xFF) != 0) g_sim_state.ctrl_pft_set = true;
        break;

    case U_ANA:
        g_sim_state.reg_ana = val;
        /* Powered up = APD (BIT0) and BPD (BIT1) both cleared.
         * 0x777: APD=BPD=1 (down). 0xFF4/0xFF0: APD=BPD=0 (up).
         * Note: 0xFF4 > 0x777 numerically, so cannot use val < 0x777. */
        if (!(val & 0x3u)) g_sim_state.ana_powered_up = true;
        /* CTATADJ[7:4] and PTATADJ[11:8] control D-PHY 100Ω termination bias.
         * Linux bcm2835-unicam.c uses ANA=0x770 (CTAT=7, PTAT=7).
         * V84: accept any non-zero CTAT/PTAT as valid (0=completely uncalibrated). */
        if (g_sim_state.ana_powered_up) {
            uint8_t ctat = (val >> 4) & 0xF;
            uint8_t ptat = (val >> 8) & 0xF;
            if (ctat == 0 && ptat == 0)
                sim_set_error("ANA: CTATADJ=0 PTATADJ=0 — D-PHY termination uncalibrated, use ANA=0x770");
        }
        break;

    case U_CLK:
        /* Lower bits: lane enable / termination.
         * V81: CLK must be configured AFTER CPR. If configured before CPR,
         * the CPR reset will wipe the configuration → STA=0 forever. */
        if ((val & 0xFFFF) != 0 && (val & 0xFFFF) != 0x02) {
            if (!g_sim_state.cpr_pulsed) {
                sim_set_error("CLK lane configured BEFORE CPR — CPR will reset it to 0x02 (power-down)! Move CLK write to after CPR.");
            }
            g_sim_state.clk_lane_enabled = true;
        }
        break;

    case U_DAT0:
        if ((val & 0xFFFF) != 0 && (val & 0xFFFF) != 0x02) {
            if (!g_sim_state.cpr_pulsed) {
                sim_set_error("DAT0 lane configured BEFORE CPR — CPR will reset it to 0x02 (power-down)! Move DAT0 write to after CPR.");
            }
            g_sim_state.dat0_lane_enabled = true;
        }
        break;

    case U_DAT1:
        if ((val & 0xFFFF) != 0 && (val & 0xFFFF) != 0x02) {
            if (!g_sim_state.cpr_pulsed) {
                sim_set_error("DAT1 lane configured BEFORE CPR — CPR will reset it to 0x02 (power-down)! Move DAT1 write to after CPR.");
            }
            g_sim_state.dat1_lane_enabled = true;
        }
        break;

    case U_CLT:
        if (val != 0) g_sim_state.clt_set = true;
        break;

    case U_DLT:
        if (val != 0) g_sim_state.dlt_set = true;
        break;

    case U_IDI0:
        g_sim_state.reg_idi0 = val;
        if (val != 0) {
            g_sim_state.idi0_set = true;
        } else {
            sim_set_error("IDI0=0 — packet filter disabled, CSI-2 engine drops all pixel data (use 0x2B for RAW10)");
        }
        break;

    case U_IPIPE:
        g_sim_state.reg_ipipe = val;
        g_sim_state.ipipe_configured = true;
        /* IPIPE=0 is valid (PUM_NONE|PPM_NONE = RAW8 passthrough per Linux) */
        break;

    case U_IBSA0:
        g_sim_state.reg_ibsa0 = val;
        if (val != 0) g_sim_state.ibsa0_set = true;
        break;

    case U_IBEA0:
        g_sim_state.reg_ibea0 = val;
        if (val != 0) g_sim_state.ibea0_set = true;
        break;

    case U_MISC:
        g_sim_state.reg_misc = val;
        if ((val & (1u<<6)) && (val & (1u<<9))) g_sim_state.misc_fl_set = true;
        break;

    case U_ICTL:
        if (val & (U_ICTL_FSIE | U_ICTL_FEIE)) g_sim_state.ictl_set = true;
        if (val & U_ICTL_LIP) {
            if (g_sim_state.cpe_enabled) {
                if (g_sim_state.ibsa0_set && g_sim_state.ibea0_set) {
                    g_sim_state.lip_triggered = true;
                } else {
                    sim_set_error("LIP triggered but IBSA0/IBEA0 not set — DMA addresses not loaded");
                }
            } else {
                sim_set_error("LIP triggered before CPE — image pointers not latched");
            }
            /* LIP is a self-clearing strobe — real hardware clears it immediately.
             * ICTL reads back as 0x07 (not 0x27) after writing 0x27. */
            s_sim_regs[U_ICTL/4] &= ~U_ICTL_LIP;
        }
        break;

    default:
        break;
    }

    /* Track CPR: it must be SET then CLEARED (pulse) */
    if (offset == U_CTRL) {
        static bool cpr_was_set = false;
        if (val & U_CTRL_CPR) {
            cpr_was_set = true;
        } else if (cpr_was_set && !(val & U_CTRL_CPR)) {
            g_sim_state.cpr_pulsed = true;
            cpr_was_set = false;
        }
    }
}

/* Read from software register file */
static uint32_t sim_on_read(uint32_t offset) {
    switch (offset) {
    case U_ISTA:
        /* Synthesize ISTA_FEI when all preconditions met and sensor streaming */
        if (g_sim_state.lip_triggered && g_sim_state.sensor_streaming &&
            g_sim_state.frame_number >= 2) {
            return s_sim_regs[U_ISTA/4] | U_ISTA_FEI | U_ISTA_FSI;
        }
        return s_sim_regs[U_ISTA/4];

    case U_STA:
        /* Real HW: STA shows FS/FE when CSI-2 protocol decoder is working.
         * STA=0 → D-PHY not syncing (termination/clock issue).
         * Synthesize FS+FE only when all preconditions met. */
        if (g_sim_state.lip_triggered && g_sim_state.sensor_streaming &&
            !g_sim_state.error_msg) {
            return s_sim_regs[U_STA/4] | U_STA_FS | U_STA_FE;
        }
        return 0;  /* STA=0: as observed on real HW when CSI-2 not syncing */

    case U_CLK:
        /* Upper 16 bits = CLKhi counter (LP-11 state count).
         * PRE-STREAM: ~49152 (CLK in LP-11 most of the time).
         * POST-STREAM: ~11520 (CLK in HS mode for active frames → fewer LP-11). */
        if (g_sim_state.sensor_streaming)
            return (s_sim_regs[U_CLK/4] & 0xFFFFu) | (0x2D00u << 16);  /* HS active */
        return (s_sim_regs[U_CLK/4] & 0xFFFFu) | (0xC000u << 16);      /* LP-11 idle */

    case U_DAT0:
        /* D0hi: POST-STREAM much higher due to LS/LE + pixel data bursts. */
        if (g_sim_state.sensor_streaming)
            return (s_sim_regs[U_DAT0/4] & 0xFFFFu) | (0xE000u << 16);
        return (s_sim_regs[U_DAT0/4] & 0xFFFFu) | (0x0A00u << 16);

    case U_DAT1:
        /* D1hi: in 2-lane mode with sensor streaming, D1 should show HS activity.
         * In 1-lane or pre-stream: LP-11 constant (0xCA00). */
        if (g_sim_state.dat1_lane_enabled && g_sim_state.sensor_streaming)
            return (s_sim_regs[U_DAT1/4] & 0xFFFFu) | (0xE000u << 16);
        return (s_sim_regs[U_DAT1/4] & 0xFFFFu) | (0xCA00u << 16);

    case U_IBWP:
        /* IBWP: after LIP triggers and sensor is streaming, DMA writes data starting
         * at IBSA0. Simulate write pointer advancing into the buffer. */
        if (g_sim_state.lip_triggered && g_sim_state.sensor_streaming)
            return g_sim_state.reg_ibsa0 + 0x600u;  /* partial frame written */
        return g_sim_state.reg_ibsa0;  /* stuck at start = no DMA activity */

    default:
        return s_sim_regs[offset/4];
    }
}

/* Macro wrappers for simulation vs hardware */
#define U_WRITE(off, val)  do { sim_on_write((off), (val)); \
                                U1[(off)/4] = (val); } while(0)
#define U_READ(off)        sim_on_read(off)
#define U_SETBITS(off, b)  U_WRITE((off), U_READ(off) | (b))
#define U_CLRBITS(off, b)  U_WRITE((off), U_READ(off) & ~(b))

#else /* HARDWARE mode */

#define U_WRITE(off, val)  U1[(off)/4] = (val)
#define U_READ(off)        U1[(off)/4]
#define U_SETBITS(off, b)  U1[(off)/4] |= (b)
#define U_CLRBITS(off, b)  U1[(off)/4] &= ~(b)

#endif /* SIMULATION */

/* ─── Diagnostic register dump (hardware only) ───────────────────────────── */
#ifndef SIMULATION
static void dump_unicam_regs(volatile uint32_t* U1) {
    uart_puts("\n=== UNICAM1 REGISTER DUMP (V76) ===\n");
    uart_puts("CTRL (0x000): "); uart_hex(U1[U_CTRL/4]);
    uart_puts(" STA (0x004): "); uart_hex(U1[U_STA/4]);   uart_puts("\n");
    uart_puts("ANA  (0x008): "); uart_hex(U1[U_ANA/4]);
    uart_puts(" CLK (0x010): "); uart_hex(U1[U_CLK/4]);   uart_puts("\n");
    uart_puts("DAT0 (0x018): "); uart_hex(U1[U_DAT0/4]);
    uart_puts(" DAT1(0x01C): "); uart_hex(U1[U_DAT1/4]);  uart_puts("\n");
    uart_puts("CLT  (0x014): "); uart_hex(U1[U_CLT/4]);
    uart_puts(" DLT (0x028): "); uart_hex(U1[U_DLT/4]);   uart_puts("\n");
    uart_puts("ICTL (0x100): "); uart_hex(U1[U_ICTL/4]);
    uart_puts(" ISTA(0x104): "); uart_hex(U1[U_ISTA/4]);  uart_puts("\n");
    uart_puts("IDI0 (0x108): "); uart_hex(U1[U_IDI0/4]);
    uart_puts(" IPIPE(0x10C):"); uart_hex(U1[U_IPIPE/4]); uart_puts("\n");
    uart_puts("IBSA0(0x110): "); uart_hex(U1[U_IBSA0/4]);
    uart_puts(" IBEA0(0x114):"); uart_hex(U1[U_IBEA0/4]); uart_puts("\n");
    uart_puts("IBLS (0x118): "); uart_hex(U1[U_IBLS/4]);
    uart_puts(" IBWP(0x11C): "); uart_hex(U1[U_IBWP/4]);  uart_puts("\n");
    uart_puts("MISC (0x400): "); uart_hex(U1[U_MISC/4]);  uart_puts("\n");
    uart_puts("===================================\n");
}
#endif

/* ─── Complete Unicam1 initialization (matches bcm2835-unicam.c unicam_start_rx) ─ */
static void setup_unicam_block(volatile uint32_t* U1) {
    /* STEP 0: Simulation state init */
#ifdef SIMULATION
    sim_init_hw_state();
    g_sim_state.frame_number = 0;
    g_sim_state.error_msg    = 0;
#endif

    /* STEP 1: CTRL = MEM only (no CPE, no CPR yet) */
    U_WRITE(U_CTRL, U_CTRL_MEM);

    /* STEP 2: ANA power-up BEFORE CPR (D-PHY must be powered before reset)
     *
     * V84 FIX: Use Linux exact ANA values: 0x774 → 0x770 (CTATADJ=PTATADJ=7).
     *
     * Linux bcm2835-unicam.c writes ANA=0x774 (power-up, hold AR, CTAT=7, PTAT=7)
     * then ANA=0x770 (release AR). CTATADJ=7 / PTATADJ=7 are the values Linux
     * uses on the SAME hardware (BCM2837 + Pi Camera v3 / IMX708).
     *
     * V79 changed to 0xFF0 (CTAT=F, PTAT=F) "for maximum D-PHY bias" — this was
     * wrong. CTAT/PTAT control the 100Ω differential termination bias current.
     * Wrong bias → wrong termination impedance → signal reflections → CSI-2 SYNC
     * (0xB8) corrupted → decoder never syncs → STA=0 forever.
     *
     * V76-V78 used the correct 0x770 but also had the CPR bug (CLK wiped), so
     * STA=0 was blamed on the wrong cause. Now that CPR is fixed (V81), reverting
     * to the Linux-verified 0x770 should be the final fix.
     *
     * Linux usleep_range(1000, 2000) after ANA write = mandatory 1-2ms for DDL lock.
     */
#ifdef SIMULATION
    {
        U_WRITE(U_ANA, 0x774u);
        delay_nop(1000000);    /* 1ms DDL lock time */
        U_WRITE(U_ANA, 0x770u);
        delay_nop(50000);
    }
#else
    {
        uint32_t vpu_ana = U1[U_ANA/4];
        uart_puts("[UNICAM] ANA (VPU): "); uart_hex(vpu_ana); uart_puts("\n");

        /* VPU always leaves ANA=0x777 (fully powered down, uncalibrated).
         * Always do full power-up with Linux-matched values: 0x774 → 0x770.
         * CTATADJ=7, PTATADJ=7 = Linux bcm2835-unicam.c exact values. */
        uart_puts("[UNICAM] ANA: power-up 0x774->0x770 (Linux CTAT=7 PTAT=7)\n");
        U1[U_ANA/4] = 0x774u;   /* power up, hold AR, CTATADJ=7, PTATADJ=7 */
        delay_nop(1000000);      /* 1ms DDL lock (Linux: usleep_range(1000, 2000)) */
        U1[U_ANA/4] = 0x770u;   /* release AR */
        delay_nop(50000);
        g_sim_state.reg_ana = U1[U_ANA/4];
        g_sim_state.ana_powered_up = true;
    }
#endif

    /* STEP 3: CPR pulse (D-PHY reset — ANA is now powered and DDL locked)
     *
     * V81 ROOT CAUSE FIX: CPR now comes BEFORE lane/timing config, matching
     * Linux unicam_start_rx(). CPR resets D-PHY frontend registers back to
     * their power-on default (0x02 = lane powered down). Therefore ALL lane
     * config (CLK, DAT0) MUST come AFTER this reset, not before.
     *
     * In V80 and earlier, CLK=0x1D/DAT0=0x1D were written BEFORE CPR.
     * After CPR, those values were reset to 0x02 (CLPD = clock lane in power-down).
     * With CLK lane in power-down, no MIPI clock recovery = STA=0 forever.
     */
    U_SETBITS(U_CTRL, U_CTRL_CPR);
    delay_nop(2000);               /* brief reset pulse (Linux: no explicit delay) */
    U_CLRBITS(U_CTRL, U_CTRL_CPR);
    U_CLRBITS(U_CTRL, U_CTRL_CPE); /* ensure CPE=0 after reset */
    delay_nop(2000);

    /* STEP 4: Full CTRL with PFT, OET timeouts
     *
     * V93 FIX: Remove V90 lane_bits (0x18). BIT(3) in CTRL is CPM (Camera Port Mode),
     * not a lane enable. CPM=0=CSI-2, CPM=1=CCP2. V90 set BIT(3)=1 → CCP2 mode →
     * Unicam tried to decode CCP2 when sensor sent CSI-2 → STA=0 in V90/V91/V92.
     *
     * Data lanes are enabled by writing 0x1D to DAT0/DAT1 registers (STEP 10 below).
     * There are NO lane-enable bits in CTRL. CTRL_BASE=0x080F02 is already correct.
     * CPM=BIT(3)=0 → CSI-2 mode ✓
     */
    U_WRITE(U_CTRL, U_CTRL_BASE);  /* = 0x080F02 (MEM|PFT=0xF|OET=128, CPM=0=CSI-2) */

    /* STEP 5: AXI bus priority */
    U_WRITE(U_PRI, 0x00000E85u);

    /* STEP 6: Disable capture window (full frame DMA, no crop) */
    U_WRITE(U_IHWIN, 0x00u);
    U_WRITE(U_IVWIN, 0x00u);

    /* STEP 7: ICTL interrupt enables + clear status registers */
    U_WRITE(U_ICTL, 0x00D80007u);  /* V103: Pi OS exact — FSIE|FEIE|IBOB + upper DMA enable bits */
    U_WRITE(U_STA,  0xFFFFFFFFu);
    U_WRITE(U_ISTA, 0x00000007u);    /* write 1 to FSI|FEI|LCI to clear */

    /* STEP 8: Timing registers — AFTER CPR (CPR does NOT reset timing registers,
     * but Linux puts them here; we follow Linux order exactly).
     * CLT1/DLT1=2 (term_en wait), CLT2/DLT2=6 (settle), DLT3=0.
     *
     * V82 ROOT CAUSE FIX #2: settle=6 (60ns at 100MHz) is REQUIRED.
     *
     * IMX708 in 1-lane mode at 1536x864 ~100fps transmits at ~1.1–1.8 Gbps.
     * At 1.1 Gbps: T_UI≈0.91ns, T_HS-ZERO-min ≈ 145ns + 10*T_UI ≈ 154ns.
     * The SYNC byte (0xB8, 8 bits) arrives at t>154ns after LP→HS transition.
     *
     * With settle=21 (210ns at 100MHz): receiver arms at 210ns → SYNC byte
     * (arriving at 154–161ns) has already passed → CSI-2 decoder never syncs
     * → STA=0 forever, even though physical MIPI (CLKhi, D0hi) is active.
     *
     * With settle=6 (60ns at 100MHz): receiver arms at 60ns → still in the
     * HS-0 preamble (zeros) → sees SYNC (0xB8) when it arrives at ~154ns ✓
     *
     * Linux bcm2835-unicam.c uses 0x0602 exactly. We now match it.
     * V79–V81 used 0x1502 ("robustness") — actually BROKE high-speed sync.
     * Note: In V76–V78 (settle=6), CLK was wiped by CPR → different failure.
     * V82 with both fixes: settle=6 + CLK after CPR → should produce STA>0.
     */
    U_WRITE(U_CLT, 0x0602u);   /* CLT1=2, CLT2=6 (60ns settle — Linux exact) */
    U_WRITE(U_DLT, 0x0602u);   /* DLT1=2, DLT2=6, DLT3=0 */

    /* STEP 9: CMP0 — secondary Frame End detection via STA.PI0=BIT(15) */
    U_WRITE(U_CMP0, 0x80000301u);

    /* STEP 10: Lane configuration — *** MUST BE AFTER CPR ***
     *
     * V81: Moved here from before CPR (was step 3 in V80).
     * CPR reset CLK/DAT0 to VPU boot default (0x02 = lane powered down).
     * We now configure lanes AFTER CPR so the configuration survives.
     *
     * V103: Pi OS CLK/DAT = 0x0005 (CLE|CLLPE, no CLHSE/CLTRE).
     *   BIT(0) = CLE   = Clock Lane Enable
     *   BIT(2) = CLLPE = Clock Lane LP Receive Enable
     * IMX708 uses non-continuous HS clock (0x0310=0x00) → CLK=0x0005 matches Pi OS.
     * No CLHSE/CLTRE: use_lp_clock=true in bcm2835-unicam.c skips HS bits on CLK lane.
     *
     * V82 diagnostic: print CLK value BEFORE our write to prove CPR reset it.
     * Expected: CLK=0x00000002 (power-down default) if CPR works correctly.
     */
#ifndef SIMULATION
    uart_puts("[UNICAM] CLK after CPR (pre-write): ");
    uart_hex(U1[U_CLK/4]);
    uart_puts("\n");
#endif
    U_WRITE(U_CLK,  0x0005u);   /* V103: CLE|CLLPE — Pi OS exact (non-continuous HS clk) */
    U_WRITE(U_DAT0, 0x0005u);   /* V103: DLE|DLLPE — Pi OS exact */
    U_WRITE(U_DAT1, 0x0005u);   /* V103: 2-lane — Pi OS exact */
    U_WRITE(U_DAT2, 0x00u);
    U_WRITE(U_DAT3, 0x00u);

    /* STEP 11: DMA buffer addresses
     * IBSA0: bus address with 0xC0000000 VideoCore bus alias.
     * Pi OS IBWP=0xCBD95000 confirms VC DMA uses 0xC0000000|phys, not 0x40000000.
     * IBEA0: exclusive end = start + total_bytes
     * IBLS:  line stride in bytes
     */
    const uint32_t phys_addr = (uint32_t)(uintptr_t)g_raw_frame;
    const uint32_t bus_addr  = 0xC0000000u | phys_addr;   /* V105: VC bus alias */
    U_WRITE(U_IBLS,  FRAME_W);              /* bytes per line — before IBSA0/IBEA0 */
    U_WRITE(U_IBSA0, bus_addr);
    U_WRITE(U_IBEA0, bus_addr + FRAME_SZ);

    /* STEP 12: Image pipeline — IPIPE then IDI0 (Linux order)
     * V91: IDI0=0x2B (DT=RAW10). IMX708 has no RAW8 mode — k_imx708_common sets
     * 0x0112/0x0113=0x0A (RAW10). 52fps at 450Mbps/2-lane confirms 10-bit pixels.
     * IPIPE=0 = RAW passthrough (same for RAW10 as for RAW8). */
    U_WRITE(U_IPIPE, 0x00u);                    /* PUM_NONE|PPM_NONE = passthrough */
    U_WRITE(U_IDI0, (0u << 6) | UNICAM_DT_RAW10); /* VC=0, DT=RAW10 — IMX708 always 0x2B */

    /* STEP 13: MISC — frame limit bits FL0=BIT(6), FL1=BIT(9) = 0x240 */
    U_SETBITS(U_MISC, (1u << 6) | (1u << 9));

    /* STEP 14: CPE — enable the peripheral */
    U_SETBITS(U_CTRL, U_CTRL_CPE);   /* CTRL = CTRL_BASE | CPE = 0x080F03 */

    /* STEP 14B: CLKGATE — write AFTER CPE (V107: matches Linux unicam_start_rx order)
     * Linux: CPR → registers → CPE → clk_write() → MISC → LIP.
     * V93 had this in Step 0B (before CPR) — wrong ordering.
     * V106: address corrected to 0x3F802004 (CSI1, not CSI0).
     * Value: 2-lane shift+OR = 0x5A000015. */
    {
        const uint32_t clkgate_val = 0x5A000015u;  /* 2-lane correct value */
#ifndef SIMULATION
        *UNICAM1_CLKGATE = clkgate_val;
        asm volatile("dsb st" ::: "memory");
        uint32_t cg_after = *UNICAM1_CLKGATE;
        uart_puts("[UNICAM] CLKGATE post-CPE: wrote=0x5A000015 readback=");
        uart_hex(cg_after); uart_puts("\n");
#else
        g_sim_state.clkgate_enabled = true;
        g_sim_state.reg_clkgate = clkgate_val;
        uart_puts("[UNICAM] CLKGATE=0x5A000015 (2-lane, post-CPE per Linux order)\n");
#endif
    }

    /* STEP 15: MISC again after CPE (Linux re-asserts FL0|FL1 after CPE) */
    U_SETBITS(U_MISC, (1u << 6) | (1u << 9));

    /* STEP 16: LIP — Load Image Pointers (MUST be AFTER CPE)
     * LIP is BIT(5) in ICTL = 0x20. Self-clearing strobe.
     * Latches IBSA0/IBEA0 into active DMA address registers.
     */
    U_SETBITS(U_ICTL, U_ICTL_LIP);   /* ICTL = 0x00D80007 | 0x20 = 0x00D80027 */

    uart_puts("[UNICAM] V111 init complete. CTRL=");
    uart_hex(U_READ(U_CTRL));
    uart_puts(" IDI0=");
    uart_hex(U_READ(U_IDI0));
    uart_puts(" IPIPE=");
    uart_hex(U_READ(U_IPIPE));
    uart_puts(" IBSA0=");
    uart_hex(U_READ(U_IBSA0));
    uart_puts("\n");
    uart_puts("[UNICAM] IBEA0=");
    uart_hex(U_READ(U_IBEA0));
    uart_puts(" MISC=");
    uart_hex(U_READ(U_MISC));
    uart_puts(" ICTL=");
    uart_hex(U_READ(U_ICTL));
    uart_puts(" CLK=");
    uart_hex(U_READ(U_CLK));
    uart_puts("\n");
    uart_puts("[UNICAM] CLT=");
    uart_hex(U_READ(U_CLT));
    uart_puts(" DLT=");
    uart_hex(U_READ(U_DLT));
    uart_puts(" ANA=");
    uart_hex(U_READ(U_ANA));
    uart_puts("\n");
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

void unicam_init() {
#ifdef SIMULATION
    /* V108: Set firmware state for simulation — these represent the mailbox
     * interactions that happen before any MMIO register setup. */
    g_sim_state.domain_powered = true;
    g_sim_state.core_clk_set = true;
    g_sim_state.cam1clk_enabled = true;
    uart_puts("[SIM] Firmware: SET_DOMAIN_STATE(domain=14, on=1) — Unicam1 powered\n");
    uart_puts("[SIM] Firmware: SET_CLOCK_RATE(clock=4, rate=250MHz) — CORE clock set\n");
#else
    /* ── Step 0: VPU firmware: power on Unicam1 domain (V108) ────────────────
     * V108 FIX: Linux bcm2835-unicam.c uses pm_runtime → raspberrypi-genpd →
     * SET_DOMAIN_STATE (tag 0x00038030), NOT the old SET_POWER_STATE (0x00028001).
     * SET_POWER_STATE always returned state=0x02 — wrong tag namespace entirely.
     *
     * domain=14 = RPI_POWER_DOMAIN_UNICAM1 (DT index 13 + 1 in raspberrypi-power.h).
     *
     * Without this, the CSI-2 decoder hardware block is power-gated:
     *   - MMIO is readable/writable (register fabric is always-on)
     *   - D-PHY analog pads are active (D-PHY is separate power rail)
     *   - But the digital CSI-2 protocol decoder is frozen (no clock/power)
     *   → STA=0, ISTA=0, IBWP stuck even with perfect register sequence
     *
     * Also: SET_CLOCK_RATE(clock_id=4, rate=250MHz) for CORE clock.
     * Linux DT specifies firmware_clocks = <4> for bcm2835-unicam node.
     */
    {
        /* GET_DOMAIN_STATE(domain=14) — diagnostic readback */
        mbox[0] = 8 * 4; mbox[1] = 0;
        mbox[2] = 0x00030030; mbox[3] = 8; mbox[4] = 0;
        mbox[5] = 14; mbox[6] = 0; mbox[7] = 0;
        mbox_flush_to_vc(8);
        int get_ok = mbox_call(8);
        uart_puts("[UNICAM] V108 DOMAIN GET resp="); uart_hex(mbox[1]);
        uart_puts(" state="); uart_hex(mbox[6]);
        uart_puts(get_ok ? " OK\n" : " FAILED\n");

        /* SET_DOMAIN_STATE(domain=14, on=1) */
        mbox[0] = 8 * 4; mbox[1] = 0;
        mbox[2] = 0x00038030; mbox[3] = 8; mbox[4] = 0;
        mbox[5] = 14; mbox[6] = 1; mbox[7] = 0;
        mbox_flush_to_vc(8);
        int set_ok = mbox_call(8);
        uart_puts("[UNICAM] V108 DOMAIN SET resp="); uart_hex(mbox[1]);
        uart_puts(" state="); uart_hex(mbox[6]);
        uart_puts(set_ok ? " OK\n" : " FAILED\n");

        /* SET_CLOCK_RATE(clock_id=4, rate=250MHz) — CORE clock */
        mbox[0] = 9 * 4; mbox[1] = 0;
        mbox[2] = 0x00038002; mbox[3] = 12; mbox[4] = 0;
        mbox[5] = 4; mbox[6] = 250000000; mbox[7] = 0; mbox[8] = 0;
        mbox_flush_to_vc(9);
        int clk_ok = mbox_call(8);
        uart_puts("[UNICAM] V108 CLK_RATE SET resp="); uart_hex(mbox[1]);
        uart_puts(" rate="); uart_hex(mbox[6]);
        uart_puts(clk_ok ? " OK\n" : " FAILED\n");
    }

    /* ── Step 0A: Configure CM_CAM1CTL (Unicam1 digital backend clock) ─────
     * Linux driver enables this via clk_prepare_enable(dev->clock).
     * Without it: CSI-2 protocol decoder has no clock → STA=0, IBWP stuck.
     * Read current value first — print for diagnostics, then configure 100 MHz.
     * Source: PLLD=6 (500 MHz), DIVI=5 → 100 MHz.
     */
    uart_puts("[UNICAM] CM_CAM1CTL before: "); uart_dec((int)*CM_CAM1CTL);
    uart_puts(" CM_CAM1DIV: "); uart_dec((int)*CM_CAM1DIV); uart_puts("\n");

    /* Stop CM_CAM1 before changing divisor (BCM2835 clock manager requirement) */
    *CM_CAM1CTL = CM_PASSWD | 6u;          /* SRC=PLLD, ENAB=0 */
    for (volatile int i = 0; i < 100000; i++) {
        if (!(*CM_CAM1CTL & (1u << 7))) break;  /* wait BUSY=0 */
        asm volatile("nop");
    }
    *CM_CAM1DIV = CM_PASSWD | (5u << 12);  /* DIVI=5 → 500/5 = 100 MHz */
    *CM_CAM1CTL = CM_PASSWD | (1u << 4) | 6u;  /* ENAB=1, SRC=PLLD */
    for (volatile int i = 0; i < 100000; i++) asm volatile("nop");  /* settle */

    uart_puts("[UNICAM] CM_CAM1CTL after:  "); uart_dec((int)*CM_CAM1CTL);
    uart_puts(" (BUSY="); uart_dec((int)((*CM_CAM1CTL >> 7) & 1));
    uart_puts(")\n");
#endif

    /* ── Step 0B: CLKGATE diagnostic pre-read (V107: actual write moved to Step 14B) */
#ifndef SIMULATION
    {
        uint32_t cg_before = *UNICAM1_CLKGATE;
        uart_puts("[UNICAM] CLKGATE pre-read: "); uart_hex(cg_before); uart_puts("\n");
    }
#endif

    volatile uint32_t* U1 = (volatile uint32_t*)(uintptr_t)UNICAM1_BASE;
    setup_unicam_block(U1);
}

bool unicam_capture_frame() {
    g_sim_state.frame_number++;

#ifdef SIMULATION
    /* ── Simulation capture logic ─────────────────────────────────────────── */
    uart_puts("[SIM] === Frame ");
    uart_dec(g_sim_state.frame_number);
    uart_puts(" capture attempt ===\n");

    /* Validate all preconditions */
    bool ok = true;

    /* V108: Firmware interactions — top-level gates */
    if (!g_sim_state.domain_powered) {
        uart_puts("[SIM] FAIL: SET_DOMAIN_STATE(domain=14) never called — Unicam1 power-gated!\n");
        uart_puts("[SIM]   HW symptom: all registers writable, D-PHY HS active, but STA=0 forever\n");
        uart_puts("[SIM]   Fix: mailbox tag 0x00038030, domain=14, on=1 (NOT old SET_POWER_STATE)\n");
        ok = false;
    }
    if (!g_sim_state.core_clk_set) {
        uart_puts("[SIM] FAIL: SET_CLOCK_RATE(clock=4, 250MHz) never called — CORE clock may be too slow\n");
        ok = false;
    }

    if (!g_sim_state.cam1clk_enabled) {
        uart_puts("[SIM] FAIL: CM_CAM1CTL (0x3F101048) not configured — Unicam1 digital backend clock OFF\n");
        uart_puts("[SIM]   STA=0 / IBWP stuck = clock frozen (same symptom as real HW V76/V77)\n");
        ok = false;
    }
    if (!g_sim_state.clkgate_enabled) {
        uart_puts("[SIM] FAIL: CLKGATE not written — Unicam1 clock domain GATED OFF\n");
        ok = false;
    }
    if (!g_sim_state.mem_bit_set) {
        uart_puts("[SIM] FAIL: CTRL.MEM never set — DMA interface disabled\n");
        ok = false;
    }
    if (!g_sim_state.ana_powered_up) {
        uart_puts("[SIM] FAIL: ANA APD/BPD bits still set (D-PHY fully powered down) — CSI-2 RX blind\n");
        ok = false;
    }
    /* Check ANA termination bias (CTATADJ[7:4] and PTATADJ[11:8]).
     * Linux bcm2835-unicam.c: ANA=0x770 (CTAT=7, PTAT=7). V84: any non-zero OK. */
    {
        uint8_t ctat = (g_sim_state.reg_ana >> 4) & 0xF;
        uint8_t ptat = (g_sim_state.reg_ana >> 8) & 0xF;
        if (g_sim_state.ana_powered_up && ctat == 0 && ptat == 0) {
            uart_puts("[SIM] FAIL: ANA CTATADJ=0 PTATADJ=0 — termination uncalibrated\n");
            uart_puts("[SIM]   Fix: write ANA=0x774 then ANA=0x770 (Linux exact)\n");
            ok = false;
        }
    }
    if (g_sim_state.cpr_before_ana) {
        uart_puts("[SIM] FAIL: CPR issued BEFORE ANA powered up — analog reset never completed\n");
        ok = false;
    }
    if (!g_sim_state.cpr_pulsed) {
        uart_puts("[SIM] FAIL: CPR never pulsed — CSI-2 protocol decoder still in reset\n");
        ok = false;
    }
    if (!g_sim_state.clk_lane_enabled) {
        uart_puts("[SIM] FAIL: CLK lane lower bits = 0 — clock lane disabled\n");
        uart_puts("[SIM]   (If CLK was written before CPR, CPR reset it to power-down!)\n");
        ok = false;
    }
    if (!g_sim_state.dat0_lane_enabled) {
        uart_puts("[SIM] FAIL: DAT0 lane lower bits = 0 — data lane disabled\n");
        uart_puts("[SIM]   (If DAT0 was written before CPR, CPR reset it to power-down!)\n");
        ok = false;
    }
    /* 2-lane validation: if sensor configured for 2 lanes, DAT1 must be enabled */
    if (g_sim_state.sensor_lane_count >= 2 && !g_sim_state.dat1_lane_enabled) {
        uart_puts("[SIM] FAIL: Sensor in 2-lane mode (0x0114=0x01) but DAT1 not enabled\n");
        uart_puts("[SIM]   Sensor sees no 100Ohm termination on D1 -> keeps both lanes LP-11\n");
        uart_puts("[SIM]   Fix: write DAT1=0x1D (after CPR, same as DAT0)\n");
        ok = false;
    }
    /* V93: CLKGATE must use shift+OR algorithm with 0x5A000000 password.
     * Expected: 1-lane=0x5A000005, 2-lane=0x5A000015 */
    if (g_sim_state.clkgate_enabled) {
        uint32_t expected_cg = 0x5A000000u;
        uint32_t val = 1u;
        for (int i = 0; i < (int)g_sim_state.sensor_lane_count; i++)
            val = (val << 2) | 1u;
        expected_cg |= val;
        if (g_sim_state.reg_clkgate != expected_cg) {
            uart_puts("[SIM] WARN: CLKGATE="); uart_hex(g_sim_state.reg_clkgate);
            uart_puts(" expected="); uart_hex(expected_cg);
            uart_puts(" (shift+OR algo: val=1; for each lane: val=(val<<2)|1)\n");
        }
    }
    if (!g_sim_state.clt_set) {
        uart_puts("[SIM] FAIL: CLT not written — clock timing undefined\n");
        ok = false;
    }
    if (!g_sim_state.dlt_set) {
        uart_puts("[SIM] FAIL: DLT not written — data timing undefined (ths_settle=0)\n");
        ok = false;
    }
    if (!g_sim_state.ctrl_pft_set) {
        uart_puts("[SIM] FAIL: PFT=0 in CTRL — packet framer may time out before FS packet\n");
        ok = false;
    }
    if (!g_sim_state.idi0_set) {
        uart_puts("[SIM] FAIL: IDI0=0 — CSI-2 engine only sees FS short pkts, ignores RAW8!\n");
        ok = false;
    }
    if (!g_sim_state.ipipe_configured) {
        uart_puts("[SIM] WARN: IPIPE never written — using VPU-default value (unknown)\n");
        /* Not fatal — could be correct by chance */
    }
    if (!g_sim_state.ibsa0_set) {
        uart_puts("[SIM] FAIL: IBSA0=0 — DMA has no target address\n");
        ok = false;
    }
    if (!g_sim_state.ibea0_set) {
        uart_puts("[SIM] FAIL: IBEA0=0 — DMA end address never set!\n");
        ok = false;
    }
    if (!g_sim_state.misc_fl_set) {
        uart_puts("[SIM] FAIL: MISC FL0|FL1 not set — frame limit control broken\n");
        ok = false;
    }
    if (!g_sim_state.ictl_set) {
        uart_puts("[SIM] FAIL: ICTL interrupt enables not set — ISTA_FEI will never be seen\n");
        ok = false;
    }
    if (!g_sim_state.cpe_enabled) {
        uart_puts("[SIM] FAIL: CPE never set — Unicam peripheral not enabled\n");
        ok = false;
    }
    /* V93: Verify CTRL CPM field = 0 (CSI-2 mode, not CCP2).
     * BIT(3) = CPM_MASK. CPM=0=CSI-2, CPM=1=CCP2.
     * V90 accidentally set BIT(3)=1 (CCP2) by adding "lane_bits=0x18". */
    if (g_sim_state.reg_ctrl & (1u << 3)) {
        uart_puts("[SIM] FAIL: CTRL CPM=1 (CCP2 mode)! Must be CPM=0 (CSI-2).\n");
        uart_puts("[SIM]   CTRL="); uart_hex(g_sim_state.reg_ctrl);
        uart_puts("  BIT(3)=1 means Unicam is decoding CCP2, not MIPI CSI-2.\n");
        uart_puts("[SIM]   Fix: CTRL = U_CTRL_BASE (0x080F02), no extra BITs.\n");
        ok = false;
    }
    if (!g_sim_state.lip_triggered) {
        uart_puts("[SIM] FAIL: LIP never triggered — DMA address registers not latched\n");
        ok = false;
    }
    if (!g_sim_state.sensor_streaming) {
        uart_puts("[SIM] FAIL: Sensor stream_on() never called — no MIPI transmission\n");
        ok = false;
    }

    if (g_sim_state.error_msg) {
        uart_puts("[SIM] First ordering error: ");
        uart_puts(g_sim_state.error_msg);
        uart_puts("\n");
        ok = false;
    }

    if (!ok) {
        uart_puts("[SIM] === SEQUENCE INVALID — fix errors above for ISTA_FEI to fire ===\n");
        return false;
    }

    /* All checks passed — synthesize successful capture */
    (void)sizeof(g_raw_frame);  /* ensure buffer is referenced */
    uart_puts("[SIM] === ALL PRECONDITIONS MET — synthesizing ISTA_FEI ===\n");
    uart_puts("[SIM]   IDI0="); uart_hex(g_sim_state.reg_idi0);
    uart_puts("  IPIPE=");      uart_hex(g_sim_state.reg_ipipe);
    uart_puts("  IBSA0=");      uart_hex(g_sim_state.reg_ibsa0);
    uart_puts("  IBEA0=");      uart_hex(g_sim_state.reg_ibea0);
    uart_puts("\n");

    /* Fill frame buffer with synthetic RAW10-packed RGGB test pattern.
     * CSI-2 RAW10 packed: every 5 bytes hold 4 pixels (8 MSBs in bytes 0-3,
     * 2 LSBs packed into byte 4). Debayer sees: RGGB pattern at 1536x864. */
    for (int y = 0; y < FRAME_H; y++) {
        uint8_t* row = g_raw_frame + y * FRAME_W;
        /* Each group of 5 bytes covers 4 horizontal pixels */
        for (int grp = 0; grp < FRAME_W / 5; grp++) {
            int px = grp * 4;  /* pixel column (0, 4, 8, ...) */
            uint16_t p[4];
            for (int k = 0; k < 4; k++) {
                int col = px + k;
                bool isR = ((y & 1) == 0) && ((col & 1) == 0);
                bool isB = ((y & 1) == 1) && ((col & 1) == 1);
                uint16_t v;
                if (isR)      v = (200 + (col % 55)) << 2;
                else if (isB) v = (50  + (y   % 50)) << 2;
                else          v = (120 + ((col + y) % 30)) << 2;
                p[k] = v;  /* 10-bit pixel value */
            }
            row[grp*5+0] = (uint8_t)(p[0] >> 2);
            row[grp*5+1] = (uint8_t)(p[1] >> 2);
            row[grp*5+2] = (uint8_t)(p[2] >> 2);
            row[grp*5+3] = (uint8_t)(p[3] >> 2);
            row[grp*5+4] = (uint8_t)(((p[0]&3)<<6)|((p[1]&3)<<4)|((p[2]&3)<<2)|(p[3]&3));
        }
    }
    uart_puts("[SIM] Synthetic RAW10-packed RGGB frame written (1536x864).\n");
    return true;

#else
    /* ── Hardware capture: poll ISTA_FEI or STA.PI0, then freeze DMA ──── */
    volatile uint32_t* U1 = (volatile uint32_t*)(uintptr_t)UNICAM1_BASE;

    /* V110: Re-enable DMA if it was stopped after previous capture.
     * Must re-assert CPE, CLKGATE, MISC, and LIP to restart reception. */
    restart_unicam_dma();

    /* V109: Clear stale status flags before this frame's capture. */
    U1[U_STA/4]  = 0xFFFFFFFFu;
    U1[U_ISTA/4] = 0xFFFFFFFFu;

    /* V110: Wait for Frame Start, then wait for Frame End.
     * This ensures we capture one complete frame from FS to FE.
     * At 52fps, FS→FE is ~19ms. Total wait ≤ ~40ms typical. */
    bool saw_fs = false;
    for (unsigned long i = 0; i < 45000000UL; i++) {
        if (i % 100000 == 0) watchdog_kick();

        uint32_t ista = U1[U_ISTA/4];
        uint32_t sta  = U1[U_STA/4];

        /* Wait for Frame Start first */
        if (!saw_fs) {
            if (ista & U_ISTA_FSI) {
                saw_fs = true;
                /* Clear all flags so we can detect THIS frame's end */
                U1[U_STA/4]  = 0xFFFFFFFFu;
                U1[U_ISTA/4] = 0xFFFFFFFFu;
            }
            continue;
        }

        /* After FS: poll for Frame End (FEI or PI0) */
        if ((ista & U_ISTA_FEI) || (sta & U_STA_PI0)) {
            stop_unicam_dma();         /* V110: freeze buffer IMMEDIATELY */
            invalidate_frame_dcache();
            uart_puts("[UNICAM] V111: ");
            uart_puts((ista & U_ISTA_FEI) ? "FEI" : "PI0");
            uart_puts(" captured, DMA stopped\n");

            /* V111 diagnostics: verify frame data format */
            uart_puts("[DIAG] IBWP=");
            uart_hex(U1[U_IBWP/4]);
            uart_puts(" IBSA0=");
            uart_hex(U1[U_IBSA0/4]);
            uart_puts(" delta=");
            uart_dec((int)(U1[U_IBWP/4] - U1[U_IBSA0/4]));
            uart_puts(" expected=");
            uart_dec(FRAME_SZ);
            uart_puts("\n");

            /* Hex dump first 20 bytes of frame buffer */
            uart_puts("[DIAG] raw[0..19]: ");
            for (int b = 0; b < 20; b++) {
                uart_hex(g_raw_frame[b]);
                uart_puts(" ");
            }
            uart_puts("\n");

            /* Hex dump bytes at row 100 offset 0..19 */
            uart_puts("[DIAG] raw[row100,0..19]: ");
            for (int b = 0; b < 20; b++) {
                uart_hex(g_raw_frame[100 * FRAME_W + b]);
                uart_puts(" ");
            }
            uart_puts("\n");

            /* Check if frame is all zeros (cache invalidation didn't work) */
            int nonzero = 0;
            for (int b = 0; b < 100; b++)
                if (g_raw_frame[b * FRAME_W + b] != 0) nonzero++;
            uart_puts("[DIAG] nonzero_sample=");
            uart_dec(nonzero);
            uart_puts("/100\n");

            /* Decode a few RGGB pixels to check color separation */
            /* At pixel (336,0): should be R position in RGGB */
            {
                const uint8_t* row0 = g_raw_frame;
                const uint8_t* row1 = g_raw_frame + FRAME_W;
                int col = 336;  /* CROP_X from debayer */
                int grp = col >> 2;
                int pos = col & 3;
                uint8_t R  = row0[grp * 5 + pos];
                uint8_t Gr = row0[grp * 5 + (pos + 1)];
                int grp1 = col >> 2;
                int pos1 = col & 3;
                uint8_t Gb = row1[grp1 * 5 + pos1];
                uint8_t B  = row1[grp1 * 5 + (pos1 + 1)];
                uart_puts("[DIAG] RGGB@(336,0): R=");
                uart_dec(R);
                uart_puts(" Gr="); uart_dec(Gr);
                uart_puts(" Gb="); uart_dec(Gb);
                uart_puts(" B="); uart_dec(B);
                uart_puts("\n");
            }
            /* Another sample at (400,200) */
            {
                const uint8_t* row0 = g_raw_frame + 200 * FRAME_W;
                const uint8_t* row1 = g_raw_frame + 201 * FRAME_W;
                int col = 400;
                int grp = col >> 2;
                int pos = col & 3;
                uint8_t R  = row0[grp * 5 + pos];
                uint8_t Gr = row0[grp * 5 + (pos + 1)];
                uint8_t Gb = row1[grp * 5 + pos];
                uint8_t B  = row1[grp * 5 + (pos + 1)];
                uart_puts("[DIAG] RGGB@(400,200): R=");
                uart_dec(R);
                uart_puts(" Gr="); uart_dec(Gr);
                uart_puts(" Gb="); uart_dec(Gb);
                uart_puts(" B="); uart_dec(B);
                uart_puts("\n");
            }

            return true;
        }

        /* Periodic diagnostic every ~100ms */
        if (i > 0 && i % 10000000 == 0) {
            uart_puts("[UNICAM] t="); uart_dec((int)(i / 10000000));
            uart_puts("00ms: STA="); uart_hex(sta);
            uart_puts(" ISTA="); uart_hex(ista);
            uart_puts(" WP="); uart_hex(U1[U_IBWP/4]);
            uart_puts(saw_fs ? " (post-FS)" : " (pre-FS)");
            uart_puts("\n");
        }
    }

    uart_puts("[UNICAM] TIMEOUT — no complete frame in ~500ms\n");
    uart_puts("[UNICAM] IBWP="); uart_hex(U1[U_IBWP/4]);
    uart_puts("  STA="); uart_hex(U1[U_STA/4]);
    uart_puts("  ISTA="); uart_hex(U1[U_ISTA/4]); uart_puts("\n");
    dump_unicam_regs(U1);
    return false;
#endif
}

void unicam_print_lane_state(const char* tag) {
#ifndef SIMULATION
    volatile uint32_t* U1 = (volatile uint32_t*)(uintptr_t)UNICAM1_BASE;
#endif
    uart_puts(tag);
    uart_puts(" CLKhi="); uart_dec((int)(U_READ(U_CLK) >> 16));
    uart_puts(" D0hi=");  uart_dec((int)(U_READ(U_DAT0) >> 16));
    uart_puts(" D1hi=");  uart_dec((int)(U_READ(U_DAT1) >> 16));
    uart_puts(" ISTA=");  uart_dec((int)(U_READ(U_ISTA)));
    uart_puts(" WP=");  uart_hex(U_READ(U_IBWP));
    uart_puts("\n");
}

const uint8_t* unicam_frame_ptr() { return g_raw_frame; }
int unicam_frame_w() { return FRAME_W; }      /* byte stride (1920) */
int unicam_frame_h() { return FRAME_H; }      /* 864 */
int unicam_pixel_w() { return 1536; }         /* actual pixel columns */
