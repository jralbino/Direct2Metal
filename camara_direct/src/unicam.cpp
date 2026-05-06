/* Unicam1 CSI-2 receiver (BCM2837) — V150 full-mode 4608x2592 init.
 *
 * Register offsets per parent src/hardware_sim.h:
 *   CTRL=0x000, STA=0x004, ANA=0x008, PRI=0x00C, CLK=0x010, CLT=0x014,
 *   DAT0=0x018, DAT1=0x01C, DAT2=0x020, DAT3=0x024, DLT=0x028, CMP0=0x02C,
 *   ICTL=0x100, ISTA=0x104, IDI0=0x108, IPIPE=0x10C,
 *   IBSA0=0x110, IBEA0=0x114, IBLS=0x118, IBWP=0x11C,
 *   IHWIN=0x120, IVWIN=0x128, MISC=0x400
 *
 * CPR is BIT(2) in CTRL (not a separate register). DAT0/DAT1 carry
 * CLE|CLLPE=0x0005 (Pi OS / bcm2835-unicam.c use_lp_clock=true for IMX708).
 * Bus alias for DMA is 0xC0000000 (VideoCore bus, L2 + SDRAM coherent path).
 */
#include "unicam.h"
#include "uart.h"
#include <stdint.h>

#define MMIO_BASE       0x3F000000UL
#define UNICAM1_BASE    (MMIO_BASE + 0x801000UL)
#define CM_PASSWD       0x5A000000u
#define UNICAM1_CLKGATE (*(volatile uint32_t*)0x3F802004UL)
#define CM_CAM1CTL      (*(volatile uint32_t*)0x3F101048UL)
#define CM_CAM1DIV      (*(volatile uint32_t*)0x3F10104CUL)

/* Unicam register offsets (verified on HW, V148) */
#define U_CTRL   0x000
#define U_STA    0x004
#define U_ANA    0x008
#define U_PRI    0x00C
#define U_CLK    0x010
#define U_CLT    0x014
#define U_DAT0   0x018
#define U_DAT1   0x01C
#define U_DAT2   0x020
#define U_DAT3   0x024
#define U_DLT    0x028
#define U_CMP0   0x02C
#define U_ICTL   0x100
#define U_ISTA   0x104
#define U_IDI0   0x108
#define U_IPIPE  0x10C
#define U_IBSA0  0x110
#define U_IBEA0  0x114
#define U_IBLS   0x118
#define U_IBWP   0x11C
#define U_IHWIN  0x120
#define U_IVWIN  0x128
#define U_MISC   0x400

/* CTRL bits */
#define U_CTRL_CPE   (1u << 0)
#define U_CTRL_MEM   (1u << 1)
#define U_CTRL_CPR   (1u << 2)
#define U_CTRL_CPM   (1u << 3)
/* Base: MEM | PFT=0xF (bits 11:8) | OET=128 (bits 22:16), CPM=0=CSI-2 */
#define U_CTRL_BASE  0x00080F02u

/* ICTL bits */
#define U_ICTL_FSIE  (1u << 0)
#define U_ICTL_FEIE  (1u << 1)
#define U_ICTL_IBOB  (1u << 2)
#define U_ICTL_LIP   (1u << 5)

/* ISTA bits — write 1 to clear */
#define U_ISTA_FSI   (1u << 0)   /* Frame Start Interrupt */
#define U_ISTA_FEI   (1u << 1)   /* Frame End Interrupt */
#define U_ISTA_LCI   (1u << 2)   /* Line Count Interrupt */

/* MISC FL0|FL1 = BIT(6)|BIT(9) */
#define U_MISC_FLBITS (((1u << 6) | (1u << 9)))

/* Full mode 4608x2592 RAW10 packed: 4608 * 10 / 8 = 5760 bytes/line.
 * Total frame = 5760 * 2592 = 14,929,920 bytes (0xE3D000 ≈ 14.24 MB). */
#define FRAME_W  5760u
#define FRAME_H  2592u
#define FRAME_SZ (FRAME_W * FRAME_H)

static volatile uint32_t* const U1 = (volatile uint32_t*)UNICAM1_BASE;

static inline uint32_t U_READ(uint32_t off)            { return U1[off/4]; }
static inline void     U_WRITE(uint32_t off, uint32_t v){ U1[off/4] = v;  }
static inline void     U_SETBITS(uint32_t off, uint32_t m){ U1[off/4] |= m; }
static inline void     U_CLRBITS(uint32_t off, uint32_t m){ U1[off/4] &= ~m;}

static void delay_nop(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) __asm__ volatile("nop");
}

/* Program CM_CAM1 digital backend clock: PLLD(500MHz) / DIVI=5 → 100 MHz.
 * Without this the CSI-2 protocol decoder has no clock → STA=0 forever.
 * This is the SAME sequence the Linux bcm2835-unicam driver performs via
 * clk_prepare_enable(dev->clock). */
static void cm_cam1_enable_100mhz() {
    /* Stop CM_CAM1 before changing divisor (BCM2835 CM requirement) */
    CM_CAM1CTL = CM_PASSWD | 6u;              /* SRC=PLLD, ENAB=0 */
    for (volatile int i = 0; i < 100000; i++) {
        if (!(CM_CAM1CTL & (1u << 7))) break; /* wait BUSY=0 */
        __asm__ volatile("nop");
    }
    CM_CAM1DIV = CM_PASSWD | (5u << 12);      /* DIVI=5 → 100MHz */
    CM_CAM1CTL = CM_PASSWD | (1u << 4) | 6u;  /* ENAB=1, SRC=PLLD */
    delay_nop(100000);
}

void unicam_init(void* buffer) {
    uart_puts("Unicam: V150 full-mode init (4608x2592)\n");

    /* STEP 0A: CM_CAM1CTL — digital backend clock (100MHz from PLLD) */
    cm_cam1_enable_100mhz();
    uart_puts("Unicam: CM_CAM1 = PLLD/5 (100MHz)\n");

    /* STEP 1: CTRL = MEM only (no CPE, no CPR yet). */
    U_WRITE(U_CTRL, U_CTRL_MEM);

    /* STEP 2: ANA power-up BEFORE CPR. Linux bcm2835-unicam.c:
     *   0x774 (CTATADJ=7, PTATADJ=7, AR held) → 1ms → 0x770 (release AR).
     * CTAT/PTAT control the 100Ω D-PHY termination bias. */
    U_WRITE(U_ANA, 0x774u);
    delay_nop(1000000);   /* 1ms DDL lock */
    U_WRITE(U_ANA, 0x770u);
    delay_nop(50000);

    /* STEP 3: CPR pulse (D-PHY reset). Lanes must be configured AFTER this. */
    U_SETBITS(U_CTRL, U_CTRL_CPR);
    delay_nop(2000);
    U_CLRBITS(U_CTRL, U_CTRL_CPR);
    U_CLRBITS(U_CTRL, U_CTRL_CPE);
    delay_nop(2000);

    /* STEP 4: Full CTRL (MEM | PFT | OET, CPM=0 = CSI-2) */
    U_WRITE(U_CTRL, U_CTRL_BASE);

    /* STEP 5: AXI priority (Linux exact) */
    U_WRITE(U_PRI, 0x00000E85u);

    /* STEP 6: Disable capture window (full-frame DMA) */
    U_WRITE(U_IHWIN, 0x00u);
    U_WRITE(U_IVWIN, 0x00u);

    /* STEP 7: ICTL + clear status */
    U_WRITE(U_ICTL, 0x00D80007u);     /* FSIE|FEIE|IBOB + Pi OS upper DMA enables */
    U_WRITE(U_STA,  0xFFFFFFFFu);
    U_WRITE(U_ISTA, 0x00000007u);

    /* STEP 8: Lane timing — Linux exact (settle=6 = 60ns at 100MHz) */
    U_WRITE(U_CLT, 0x0602u);
    U_WRITE(U_DLT, 0x0602u);

    /* STEP 9: CMP0. Live libcamera dump while streaming shows CMP0=0x80000301.
     * V134 disabled this because we thought it was firing prematurely at
     * line 704; turns out the divergence was elsewhere and CMP0 is part
     * of the proper frame-boundary protocol Linux uses. */
    U_WRITE(U_CMP0, 0x80000301u);

    /* STEP 10: Lanes AFTER CPR. Values taken DIRECTLY from a libcamera
     * stream-time devmem dump on 2026-04-26 (linux_capture_linux/unicam_run.txt):
     *   CLK  = 0x06000005   (3<<25) | CLE | CLLPE   — clock lane HS term
     *   DAT0 = 0xC0000005   (3<<30) | DLE | DLLPE   — data lane 0 HS term
     *   DAT1 = 0x06000005   (3<<25) | DLE | DLLPE   — data lane 1: LIBCAMERA
     *                                                  USES THE *CLOCK*-PATTERN
     *                                                  TERMINATION, NOT THE
     *                                                  DATA-PATTERN. Setting
     *                                                  DAT1 to 0xC0000005 like
     *                                                  DAT0 corrupts every
     *                                                  other byte on the
     *                                                  receive path — visible
     *                                                  as "line swap" on real
     *                                                  scenes but invisible
     *                                                  on uniform-content test
     *                                                  patterns (V147). Found
     *                                                  by direct register diff
     *                                                  vs running libcamera.
     *   DAT2 = 0x0A000002   unused-lane config — Linux still writes a
     *                       non-zero value here (bits 25, 27, 1).
     *   DAT3 = 0x0A000002   same as DAT2. */
    U_WRITE(U_CLK,  0x06000005u);
    U_WRITE(U_DAT0, 0xC0000005u);
    U_WRITE(U_DAT1, 0x06000005u);
    U_WRITE(U_DAT2, 0x0A000002u);
    U_WRITE(U_DAT3, 0x0A000002u);

    /* STEP 11: DMA buffer — 0xC0000000 VideoCore bus alias (V105 fix).
     * Pi OS IBWP=0xCBD95000 confirms VC DMA uses this alias, not 0x40000000.
     * IBLS first (byte stride), then IBSA0/IBEA0. */
    const uint32_t phys_addr = (uint32_t)(uintptr_t)buffer;
    const uint32_t bus_addr  = 0xC0000000u | phys_addr;
    U_WRITE(U_IBLS,  FRAME_W);
    U_WRITE(U_IBSA0, bus_addr);
    U_WRITE(U_IBEA0, bus_addr + FRAME_SZ);

    /* STEP 12: IPIPE + IDI0. IPIPE=0 = passthrough. IDI0=0x2B (RAW10, VC=0).
     * IMX708 always emits RAW10 (0x0112/0x0113=0x0A in sensor init). */
    U_WRITE(U_IPIPE, 0x00u);
    U_WRITE(U_IDI0,  (0u << 6) | 0x2Bu);

    /* STEP 13: MISC — FL0|FL1 */
    U_SETBITS(U_MISC, U_MISC_FLBITS);

    /* STEP 14: CPE — enable peripheral */
    U_SETBITS(U_CTRL, U_CTRL_CPE);

    /* STEP 14B: CLKGATE with password — AFTER CPE (Linux order). 0x15 = 2-lane. */
    UNICAM1_CLKGATE = 0x5A000015u;
    __asm__ volatile("dsb st" ::: "memory");

    /* STEP 15: MISC re-asserted after CPE (Linux re-asserts FL bits) */
    U_SETBITS(U_MISC, U_MISC_FLBITS);

    /* STEP 16: LIP — self-clearing strobe, latches IBSA0/IBEA0 into active DMA regs */
    U_SETBITS(U_ICTL, U_ICTL_LIP);

    uart_puts("Unicam: init OK. IBSA0=0x"); uart_hex(U_READ(U_IBSA0));
    uart_puts(" IBEA0=0x"); uart_hex(U_READ(U_IBEA0));
    uart_puts(" IBLS=0x"); uart_hex(U_READ(U_IBLS)); uart_puts("\n");
}

void unicam_capture_start() {
    /* All arming is already done in unicam_init (CPE+LIP).
     * The Linux-order sequence requires the sensor to call stream_on AFTER
     * unicam_init has armed CPE. main.cpp already calls imx708_stream_on()
     * then this function — we just need to ensure CPE is set and re-trigger
     * LIP so the next frame boundary latches the buffer cleanly. */
    U_SETBITS(U_CTRL, U_CTRL_CPE);
    UNICAM1_CLKGATE = 0x5A000015u;
    __asm__ volatile("dsb st" ::: "memory");
    U_SETBITS(U_MISC, U_MISC_FLBITS);
    U_SETBITS(U_ICTL, U_ICTL_LIP);
    uart_puts("Unicam: CPE + LIP re-armed after sensor stream_on\n");
}

uint32_t unicam_get_ibwp()  { return U_READ(U_IBWP); }
uint32_t unicam_get_sta()   { return U_READ(U_STA);  }
uint32_t unicam_get_ihwin() { return U_READ(U_IHWIN);}
uint32_t unicam_get_ivwin() { return U_READ(U_IVWIN);}
uint32_t unicam_get_ibls()  { return U_READ(U_IBLS); }
uint32_t unicam_get_ibsa0() { return U_READ(U_IBSA0);}
uint32_t unicam_get_ibea0() { return U_READ(U_IBEA0);}
uint32_t unicam_get_ctrl()  { return U_READ(U_CTRL); }
uint32_t unicam_get_idi0()  { return U_READ(U_IDI0); }
uint32_t unicam_get_misc()  { return U_READ(U_MISC); }
uint32_t unicam_get_ana()   { return U_READ(U_ANA);  }
uint32_t unicam_get_pri()   { return U_READ(U_PRI);  }
uint32_t unicam_get_clk()   { return U_READ(U_CLK);  }
uint32_t unicam_get_clt()   { return U_READ(U_CLT);  }
uint32_t unicam_get_dat0()  { return U_READ(U_DAT0); }
uint32_t unicam_get_dat1()  { return U_READ(U_DAT1); }
uint32_t unicam_get_dat2()  { return U_READ(U_DAT2); }
uint32_t unicam_get_dat3()  { return U_READ(U_DAT3); }
uint32_t unicam_get_dlt()   { return U_READ(U_DLT);  }
uint32_t unicam_get_cmp0()  { return U_READ(U_CMP0); }
uint32_t unicam_get_ictl()  { return U_READ(U_ICTL); }
uint32_t unicam_get_ista()  { return U_READ(U_ISTA); }
uint32_t unicam_get_ipipe() { return U_READ(U_IPIPE);}

/* Wait for a complete frame using the FS-to-FS protocol (V154).
 *
 * Linux libcamera / bcm2835-unicam.c, and the parent project's V136
 * capture_one_frame, both use this protocol: wait for two FS (Frame Start)
 * interrupts and let the data between them be the captured frame.
 *
 *   FS1 detected → trigger LIP (latches IBSA0/IBEA0, IBWP resets to IBSA0).
 *                  DMA now writes the new frame from the start of the buffer.
 *   FS2 detected → next frame is starting, so the previous one is complete.
 *                  Stop DMA (CPE=0) to freeze the buffer for a tear-free read.
 *
 * Why this is more robust than IBWP-stable polling: PDAF / embedded-data
 * short packets cause IBWP to stop moving for hundreds of µs to a few ms
 * mid-frame (data is on a different VC, filtered by IDI0). The previous
 * "5000 stable reads" heuristic mistook those stalls for VBLANK, stopping
 * DMA mid-frame and producing torn rows that debayer to magenta. The FS bit
 * is a clean, deterministic boundary aligned with the sensor's own frame
 * start signal — no false positives possible.
 *
 * The running max IBWP across the FS1→FS2 window is exposed via
 * unicam_get_ibwp_max() so the caller can zero out any tail rows that
 * the BCM2837 Unicam pre-existing 74%-coverage limit leaves unwritten. */
static uint32_t s_ibwp_max = 0;

uint32_t unicam_get_ibwp_max() { return s_ibwp_max; }

void unicam_wait_frame_end_stop() {
    s_ibwp_max = U_READ(U_IBSA0);   /* baseline: nothing written yet */

    /* Make sure DMA is running (idempotent if already so) and clear any
     * pending interrupt status from a previous frame. */
    U_SETBITS(U_CTRL, U_CTRL_CPE);
    UNICAM1_CLKGATE = 0x5A000015u;
    U_SETBITS(U_MISC, U_MISC_FLBITS);
    __asm__ volatile("dsb st" ::: "memory");
    U_WRITE(U_STA,  0xFFFFFFFFu);
    U_WRITE(U_ISTA, 0xFFFFFFFFu);

    int      fs_count = 0;
    /* ~3 s budget at ~150 ns per MMIO read = 20M iterations.
     * One frame = ~87 ms, so two frames easily fit; the loop is sized for
     * worst-case sensor-not-yet-streaming on the very first call. */
    const uint32_t kMaxIters = 20000000u;
    for (uint32_t i = 0; i < kMaxIters; i++) {
        uint32_t ista = U_READ(U_ISTA);

        /* Track running max IBWP between FS1 and FS2. Sample sparsely to
         * keep the loop tight (every 256 iters ≈ ~40 µs, well below
         * line time of 33 µs at 4608 px × 10 bit / 2 lanes). */
        if (fs_count == 1 && (i & 0xFFu) == 0u) {
            uint32_t wp = U_READ(U_IBWP);
            if (wp > s_ibwp_max) s_ibwp_max = wp;
        }

        if (ista & U_ISTA_FSI) {
            U_WRITE(U_ISTA, 0xFFFFFFFFu);   /* W1C all status bits */
            fs_count++;

            if (fs_count == 1) {
                /* FS1: latch buffer addresses, reset write pointer.
                 * Drop any pre-FS1 IBWP samples — those reflected the
                 * previous frame's tail (DMA was free-running before we
                 * arrived) and would over-report coverage for this frame. */
                U_SETBITS(U_ICTL, U_ICTL_LIP);
                s_ibwp_max = U_READ(U_IBSA0);
            } else {
                /* FS2: previous frame finished. Capture final IBWP before
                 * we stop DMA — the BCM2837 may reset IBWP to IBSA0 when
                 * CPE goes 0, so we must read it now. */
                uint32_t wp_final = U_READ(U_IBWP);
                if (wp_final > s_ibwp_max) s_ibwp_max = wp_final;
                U_CLRBITS(U_CTRL, U_CTRL_CPE);
                __asm__ volatile("dsb sy" ::: "memory");
                return;
            }
        }
    }

    /* Timeout fallback — caller sees s_ibwp_max==IBSA0 and bytes_valid==0,
     * which the V153 zero-fill turns into a fully black frame. This path
     * means the sensor is not delivering FS (no clock, standby, etc.). */
    U_CLRBITS(U_CTRL, U_CTRL_CPE);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Re-arm DMA for the next frame: CPE on, MISC FL re-asserted (the parent
 * project's restart_unicam_dma — without re-asserting FL the BCM2837 can
 * drop part of the next frame), LIP strobe to latch buffer regs. */
void unicam_rearm() {
    U_SETBITS(U_CTRL, U_CTRL_CPE);
    UNICAM1_CLKGATE = 0x5A000015u;
    U_SETBITS(U_MISC, U_MISC_FLBITS);
    __asm__ volatile("dsb st" ::: "memory");
    U_SETBITS(U_ICTL, U_ICTL_LIP);
}

/* ── V158 continuous-mode primitives ───────────────────────────────────────
 *
 * Continuous DMA via IBSA0 rotation (libcamera-aligned). The protocol:
 *
 *   1. Software writes the *next* buffer's bus address into IBSA0/IBEA0.
 *      This update lands in the register file but does NOT affect the live
 *      DMA — there's a shadow register that the DMA engine actually uses,
 *      and only LIP copies IBSA0/IBEA0 into that shadow.
 *   2. At the next sensor FS, software strobes LIP. The shadow updates,
 *      IBWP resets to the new IBSA0, and DMA writes the upcoming frame
 *      into the new buffer.
 *   3. Meanwhile, the buffer that was active *before* this LIP holds the
 *      just-completed frame and is safe to read for debayer + display.
 *
 * Compared to V154's wait_frame_end_stop the differences are:
 *   - We never clear CPE, so DMA streams continuously.
 *   - We wait for ONE FSI per frame, not two — the FS we wait for is the
 *     boundary marking the previous frame as done.
 *   - Caller pre-stages IBSA0 between calls; this function just commits it.
 */

void unicam_stage_dma_buffer(void* buf) {
    const uint32_t bus_addr = 0xC0000000u | (uint32_t)(uintptr_t)buf;
    U_WRITE(U_IBSA0, bus_addr);
    U_WRITE(U_IBEA0, bus_addr + FRAME_SZ);
    __asm__ volatile("dsb st" ::: "memory");
}

uint32_t unicam_wait_fs_and_lip() {
    /* Belt-and-braces: re-assert CPE/CLKGATE/MISC in case anything cleared
     * them (matches V154's pattern). Idempotent if already running. */
    U_SETBITS(U_CTRL, U_CTRL_CPE);
    UNICAM1_CLKGATE = 0x5A000015u;
    U_SETBITS(U_MISC, U_MISC_FLBITS);
    __asm__ volatile("dsb st" ::: "memory");

    /* W1C: clear any pending FSI from before so we wait for a *fresh* FS. */
    U_WRITE(U_ISTA, 0xFFFFFFFFu);

    /* Poll. Frame period at full mode is ~117 ms = ~780k MMIO reads at
     * 150 ns each, so 20M iters is a generous 3 s ceiling for first-call
     * sensor-not-streaming-yet. */
    const uint32_t kMaxIters = 20000000u;
    for (uint32_t i = 0; i < kMaxIters; i++) {
        if (U_READ(U_ISTA) & U_ISTA_FSI) {
            U_WRITE(U_ISTA, 0xFFFFFFFFu);     /* clear the FSI we just consumed */
            /* Sample IBWP BEFORE LIP — LIP resets IBWP to the new IBSA0,
             * losing the just-completed frame's byte count. The return
             * value is what the caller uses to compute rows_max for
             * coverage diagnostics. */
            uint32_t ibwp_pre_lip = U_READ(U_IBWP);
            U_SETBITS(U_ICTL, U_ICTL_LIP);    /* commit staged IBSA0/IBEA0 */
            __asm__ volatile("dsb sy" ::: "memory");
            return ibwp_pre_lip;
        }
    }
    /* Timeout fallback — sensor not delivering FS. Caller will see stale
     * pixels. We don't stop CPE because that would also kill the next
     * recovery attempt; instead the symptom is a stuck/dim image and the
     * UART log shows that frame timing diverged. */
    return 0u;
}
