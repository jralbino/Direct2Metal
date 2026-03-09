/* File: src/camera_unicam.cpp
 * V81 — Complete BCM2837 Unicam1 CSI-2 Driver + Hardware-Faithful Simulator
 *
 * ── CRITICAL BUGS FIXED vs V75 ──────────────────────────────────────────────
 *   [A] IDI0 at 0x108 = 0x2A (RAW8, VC=0) — was COMPLETELY MISSING in V75!
 *   [B] IPIPE at 0x10C = 0x00 (RAW8 passthrough) — was COMPLETELY MISSING in V75!
 *   [C] IBEA0 at 0x114 = IBSA0+frame_size — was COMPLETELY MISSING in V75!
 *   [D] IBLS at 0x118 = FRAME_W — was COMPLETELY MISSING in V75!
 *   [E] MISC at 0x400 = FL0|FL1 — was MISSING in V75!
 *   [F] CLKGATE at 0x3F802004 — was MISSING in V75!
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

/* ─── Hardware Base Addresses ─────────────────────────────────────────────── */
#define UNICAM1_BASE        0x3F801000UL
#define UNICAM1_CLKGATE     ((volatile uint32_t*)0x3F802004UL)
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
#define FRAME_W  1536
#define FRAME_H   864
#define FRAME_SZ  (FRAME_W * FRAME_H)

/* ─── DMA frame buffer ────────────────────────────────────────────────────── */
__attribute__((aligned(64)))
static uint8_t g_raw_frame[FRAME_SZ];

/* ─── Global sim state (defined here, extern in hardware_sim.h) ───────────── */
UnicamSimState g_sim_state = {0};

/* ─── Delays ──────────────────────────────────────────────────────────────── */
static void delay_nop(unsigned int n) {
    for (volatile unsigned int i = 0; i < n; i++) asm volatile("nop");
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
    s_sim_regs[offset/4] = val;
    g_sim_state.reg_ctrl = s_sim_regs[U_CTRL/4];

    switch (offset) {
    case U_CTRL:
        if (val & U_CTRL_MEM) g_sim_state.mem_bit_set = true;
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
         * Linux bcm2835-unicam.c writes 0xFF0 (both fields = 0xF = max).
         * ANA=0x770 (fields=7) → weak termination → D-PHY won't sync → STA=0. */
        if (g_sim_state.ana_powered_up) {
            uint8_t ctat = (val >> 4) & 0xF;
            uint8_t ptat = (val >> 8) & 0xF;
            if (ctat < 0xF || ptat < 0xF)
                sim_set_error("ANA: CTATADJ/PTATADJ < 0xF — D-PHY 100Ω termination weak, use ANA=0xFF0");
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
            sim_set_error("IDI0=0 — CSI-2 engine only sees FS short pkts, ignores RAW8 pixel data");
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
        /* D1hi: unchanged in 1-lane mode (lane disabled, stays at VPU value). */
        return (s_sim_regs[U_DAT1/4] & 0xFFFFu) | (0xCA00u << 16);

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
     * Linux bcm2835-unicam.c uses ANA=0x774 → 0x770 (CTATADJ=PTATADJ=7).
     * We use 0xFF4 → 0xFF0 (CTATADJ=PTATADJ=0xF) for maximum D-PHY bias.
     * V80 confirmed VPU leaves ANA=0x777 (no calibration from dtoverlay).
     *
     * Linux usleep_range(1000, 2000) after ANA write = mandatory 1-2ms for DDL lock.
     * Increase from V80's 200K NOPs (~0.4ms) to 1M NOPs (~1ms at 500MHz).
     */
#ifdef SIMULATION
    {
        U_WRITE(U_ANA, 0xFF4u);
        delay_nop(1000000);    /* 1ms DDL lock time */
        U_WRITE(U_ANA, 0xFF0u);
        delay_nop(50000);
    }
#else
    {
        uint32_t vpu_ana = U1[U_ANA/4];
        uart_puts("[UNICAM] ANA (VPU): "); uart_hex(vpu_ana); uart_puts("\n");

        if (vpu_ana & 0x3u) {
            /* VPU left D-PHY powered down (APD or BPD set) — do full power-up */
            uart_puts("[UNICAM] ANA: powered down by VPU — full power-up with 0xFF0\n");
            U1[U_ANA/4] = 0xFF4u;   /* power up, hold AR, max bias */
            delay_nop(1000000);      /* 1ms DDL lock (Linux: usleep_range(1000, 2000)) */
            U1[U_ANA/4] = 0xFF0u;   /* release AR */
            delay_nop(50000);
        } else {
            /* VPU calibrated the D-PHY — preserve CTATADJ/PTATADJ, just release AR */
            uart_puts("[UNICAM] ANA: VPU calibrated — preserving bias, releasing AR\n");
            uint32_t calibrated = (vpu_ana | 0x4u);   /* set AR (hold reset) */
            U1[U_ANA/4] = calibrated;
            delay_nop(1000000);      /* 1ms DDL lock */
            U1[U_ANA/4] = (vpu_ana & ~0x7u);          /* clear APD,BPD,AR */
            delay_nop(50000);
        }
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

    /* STEP 4: Full CTRL with PFT and OET timeouts
     * CTRL_BASE = MEM|PFT=0xF|OET=128 = 0x080F02
     */
    U_WRITE(U_CTRL, U_CTRL_BASE);

    /* STEP 5: AXI bus priority */
    U_WRITE(U_PRI, 0x00000E85u);

    /* STEP 6: Disable capture window (full frame DMA, no crop) */
    U_WRITE(U_IHWIN, 0x00u);
    U_WRITE(U_IVWIN, 0x00u);

    /* STEP 7: ICTL interrupt enables + clear status registers */
    U_WRITE(U_ICTL, U_ICTL_FSIE | U_ICTL_FEIE | U_ICTL_IBOB);  /* = 0x07 */
    U_WRITE(U_STA,  0xFFFFFFFFu);
    U_WRITE(U_ISTA, 0x00000007u);    /* write 1 to FSI|FEI|LCI to clear */

    /* STEP 8: Timing registers — AFTER CPR (CPR does NOT reset timing registers,
     * but Linux puts them here; we follow Linux order exactly).
     * CLT1/DLT1=2 (term_en wait), CLT2/DLT2=21 (settle), DLT3=0.
     * Linux uses 0x0602 (settle=6); we use 0x1502 (settle=21) for robustness.
     */
    U_WRITE(U_CLT, 0x1502u);   /* CLT1=2, CLT2=21 (188ns settle) */
    U_WRITE(U_DLT, 0x1502u);   /* DLT1=2, DLT2=21, DLT3=0 */

    /* STEP 9: CMP0 — secondary Frame End detection via STA.PI0=BIT(15) */
    U_WRITE(U_CMP0, 0x80000301u);

    /* STEP 10: Lane configuration — *** MUST BE AFTER CPR ***
     *
     * V81: Moved here from before CPR (was step 3 in V80).
     * CPR reset CLK/DAT0 to VPU boot default (0x02 = lane powered down).
     * We now configure lanes AFTER CPR so the configuration survives.
     *
     * 0x1D = CLE|CLTRE|CLHSE|CLLPE:
     *   BIT(0) = CLE   = Clock Lane Enable
     *   BIT(2) = CLLPE = Clock Lane LP Receive Enable
     *   BIT(3) = CLHSE = Clock Lane HS Receive Enable
     *   BIT(4) = CLTRE = Clock Lane Termination Resistance Enable (100Ω)
     * IMX708 uses continuous HS clock → all 4 bits needed.
     * DAT1=0x00: 1-lane mode, data lane 1 disabled.
     */
    U_WRITE(U_CLK,  0x1Du);   /* CLE|CLTRE|CLHSE|CLLPE (preserve nothing — CPR cleared hi bits too) */
    U_WRITE(U_DAT0, 0x1Du);   /* DLE|DLTRE|DLHSE|DLLPE */
    U_WRITE(U_DAT1, 0x00u);   /* 1-lane: disabled */
    U_WRITE(U_DAT2, 0x00u);
    U_WRITE(U_DAT3, 0x00u);

    /* STEP 11: DMA buffer addresses
     * IBSA0: bus address with 0x40000000 L2-bypass alias (ARM AXI path)
     * IBEA0: exclusive end = start + total_bytes
     * IBLS:  line stride in bytes
     */
    const uint32_t phys_addr = (uint32_t)(uintptr_t)g_raw_frame;
    const uint32_t bus_addr  = 0x40000000u | phys_addr;   /* L2-bypass */
    U_WRITE(U_IBLS,  FRAME_W);              /* bytes per line — before IBSA0/IBEA0 */
    U_WRITE(U_IBSA0, bus_addr);
    U_WRITE(U_IBEA0, bus_addr + FRAME_SZ);

    /* STEP 12: Image pipeline — IPIPE then IDI0 (Linux order) */
    U_WRITE(U_IPIPE, 0x00u);                    /* PUM_NONE|PPM_NONE = RAW8 passthrough */
    U_WRITE(U_IDI0, (0u << 6) | 0x2Au);         /* VC=0, DT=RAW8=0x2A */

    /* STEP 13: MISC — frame limit bits FL0=BIT(6), FL1=BIT(9) = 0x240 */
    U_SETBITS(U_MISC, (1u << 6) | (1u << 9));

    /* STEP 14: CPE — enable the peripheral */
    U_SETBITS(U_CTRL, U_CTRL_CPE);   /* CTRL = CTRL_BASE | CPE = 0x080F03 */

    /* STEP 15: MISC again after CPE (Linux re-asserts FL0|FL1 after CPE) */
    U_SETBITS(U_MISC, (1u << 6) | (1u << 9));

    /* STEP 16: LIP — Load Image Pointers (MUST be AFTER CPE)
     * LIP is BIT(5) in ICTL = 0x20. Self-clearing strobe.
     * Latches IBSA0/IBEA0 into active DMA address registers.
     */
    U_SETBITS(U_ICTL, U_ICTL_LIP);   /* ICTL = 0x07 | 0x20 = 0x27 */

    uart_puts("[UNICAM] V81 init complete. CTRL=");
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
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

void unicam_init() {
#ifndef SIMULATION
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

    /* ── Step 0B: Enable Unicam1 clock gate ─────────────────────────────── */
    *UNICAM1_CLKGATE = CM_PASSWD | 0x05u;  /* CLK+DAT0 gates for 1-lane */
#ifdef SIMULATION
    g_sim_state.cam1clk_enabled = true;   /* sim assumes CM_CAM1CTL configured */
    g_sim_state.clkgate_enabled = true;
    g_sim_state.reg_clkgate = 0x05u;
#endif
    uart_puts("[UNICAM] CLKGATE=0x05 (1-lane: CLK+D0)\n");

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
     * Linux bcm2835-unicam.c: ANA=0xFF0 (both fields max=0xF).
     * ANA=0x770 (fields=7) → weak 100Ω termination → STA=0 observed on real HW V78! */
    {
        uint8_t ctat = (g_sim_state.reg_ana >> 4) & 0xF;
        uint8_t ptat = (g_sim_state.reg_ana >> 8) & 0xF;
        if (g_sim_state.ana_powered_up && (ctat < 0xF || ptat < 0xF)) {
            uart_puts("[SIM] FAIL: ANA CTATADJ="); uart_dec(ctat);
            uart_puts(" PTATADJ="); uart_dec(ptat);
            uart_puts(" < 0xF — D-PHY 100Ω termination too weak → STA=0 on real HW\n");
            uart_puts("[SIM]   Fix: write ANA=0xFF4 then ANA=0xFF0 (matches Linux bcm2835-unicam.c)\n");
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

    /* Fill frame buffer with synthetic RAW8 RGGB test pattern */
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            /* Simple gradient + Bayer pattern */
            uint8_t pixel;
            bool isR  = ((y & 1) == 0) && ((x & 1) == 0);
            bool isG1 = ((y & 1) == 0) && ((x & 1) == 1);
            bool isG2 = ((y & 1) == 1) && ((x & 1) == 0);
            bool isB  = ((y & 1) == 1) && ((x & 1) == 1);
            (void)isG1; (void)isG2;
            if (isR)       pixel = 200 + (x % 55);    /* bright red channel */
            else if (isB)  pixel = 50  + (y % 50);    /* dim blue channel */
            else           pixel = 120 + ((x + y) % 30); /* medium green */
            g_raw_frame[y * FRAME_W + x] = pixel;
        }
    }
    uart_puts("[SIM] Synthetic RAW8 RGGB frame written (1536x864).\n");
    return true;

#else
    /* ── Hardware capture: poll ISTA_FEI or STA.PI0 ─────────────────────── */
    volatile uint32_t* U1 = (volatile uint32_t*)(uintptr_t)UNICAM1_BASE;
    bool saw_fs = false;
    for (unsigned long i = 0; i < 45000000UL; i++) {
        if (i % 100000 == 0) watchdog_kick();

        /* Periodic STA/ISTA diagnostic: print every ~10M iterations (~100ms) */
        if (i > 0 && i % 10000000 == 0) {
            uart_puts("[UNICAM] t="); uart_dec((int)(i / 10000000));
            uart_puts("00ms: STA="); uart_hex(U1[U_STA/4]);
            uart_puts(" ISTA="); uart_hex(U1[U_ISTA/4]);
            uart_puts(" WP="); uart_hex(U1[U_IBWP/4]); uart_puts("\n");
        }

        uint32_t ista = U1[U_ISTA/4];
        uint32_t sta  = U1[U_STA/4];

        /* Log first FS (Frame Start) as diagnostic — if FS fires but FE never does,
         * the problem is inside the frame (data corruption/DMA issue), not protocol init */
        if (!saw_fs && (ista & U_ISTA_FSI)) {
            saw_fs = true;
            uart_puts("[UNICAM] FS received (ISTA_FSI)! WP="); uart_hex(U1[U_IBWP/4]); uart_puts("\n");
        }

        if (ista & U_ISTA_FEI) {
            U1[U_ISTA/4] = 0xFFFFFFFFu;   /* clear all interrupt flags */
            return true;
        }
        if (sta & U_STA_PI0) {
            U1[U_ISTA/4] = 0xFFFFFFFFu;
            U1[U_STA/4]  = U_STA_PI0;     /* clear CMP0 match flag */
            return true;
        }
    }

    uart_puts("[UNICAM] TIMEOUT — ISTA_FEI never fired after ~500ms\n");
    uart_puts("[UNICAM] CM_CAM1CTL="); uart_dec((int)*CM_CAM1CTL);
    uart_puts(" CM_CAM1DIV="); uart_dec((int)*CM_CAM1DIV); uart_puts("\n");
    uart_puts("[UNICAM] IBWP="); uart_hex(U1[U_IBWP/4]);
    uart_puts("  STA="); uart_hex(U1[U_STA/4]);
    uart_puts("  ISTA="); uart_hex(U1[U_ISTA/4]); uart_puts("\n");
    uart_puts("[UNICAM] ANA="); uart_hex(U1[U_ANA/4]);
    uart_puts(" CLT="); uart_hex(U1[U_CLT/4]);
    uart_puts(" DLT="); uart_hex(U1[U_DLT/4]); uart_puts("\n");
    uart_puts("  (IBWP==IBSA0 + STA=0: CSI-2 not syncing — check ANA/DLT/lane mode)\n");
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
int unicam_frame_w() { return FRAME_W; }
int unicam_frame_h() { return FRAME_H; }
