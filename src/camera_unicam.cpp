/* File: src/camera_unicam.cpp
 * V76 — Complete BCM2837 Unicam1 CSI-2 Driver + Hardware-Faithful Simulator
 *
 * ── CRITICAL BUGS FIXED vs V75 ──────────────────────────────────────────────
 *   [A] IDI0 at 0x108 = 0x2A (RAW8, VC=0) — was COMPLETELY MISSING in V75!
 *       Without IDI0, the CSI-2 protocol engine ignores all pixel packets.
 *   [B] IPIPE at 0x10C = 0x00 (RAW8 passthrough) — was COMPLETELY MISSING in V75!
 *       Without IPIPE touching, DMA state may be undefined from VPU boot.
 *   [C] IBEA0 at 0x114 = IBSA0+frame_size — was COMPLETELY MISSING in V75!
 *       DMA needs end address to know when buffer is full.
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
        /* ANA < 0x777 means at least partial power-up */
        if (val < U_ANA_ALL_OFF) g_sim_state.ana_powered_up = true;
        break;

    case U_CLK:
        /* Lower bits: lane enable / termination */
        if ((val & 0xFFFF) != 0 && (val & 0xFFFF) != 0x02)
            g_sim_state.clk_lane_enabled = true;
        break;

    case U_DAT0:
        if ((val & 0xFFFF) != 0 && (val & 0xFFFF) != 0x02)
            g_sim_state.dat0_lane_enabled = true;
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
    /* Synthesize ISTA_FEI when simulation is "ready" */
    if (offset == U_ISTA && g_sim_state.lip_triggered &&
        g_sim_state.sensor_streaming && g_sim_state.frame_number >= 2) {
        /* Simulate frame end interrupt after 2nd capture attempt */
        return s_sim_regs[U_ISTA/4] | U_ISTA_FEI;
    }
    return s_sim_regs[offset/4];
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
    uart_puts("CTRL (0x000): 0x"); uart_hex(U1[U_CTRL/4]);
    uart_puts(" STA (0x004): 0x"); uart_hex(U1[U_STA/4]);   uart_puts("\n");
    uart_puts("ANA  (0x008): 0x"); uart_hex(U1[U_ANA/4]);
    uart_puts(" CLK (0x010): 0x"); uart_hex(U1[U_CLK/4]);   uart_puts("\n");
    uart_puts("DAT0 (0x018): 0x"); uart_hex(U1[U_DAT0/4]);
    uart_puts(" DAT1(0x01C): 0x"); uart_hex(U1[U_DAT1/4]);  uart_puts("\n");
    uart_puts("CLT  (0x014): 0x"); uart_hex(U1[U_CLT/4]);
    uart_puts(" DLT (0x028): 0x"); uart_hex(U1[U_DLT/4]);   uart_puts("\n");
    uart_puts("ICTL (0x100): 0x"); uart_hex(U1[U_ICTL/4]);
    uart_puts(" ISTA(0x104): 0x"); uart_hex(U1[U_ISTA/4]);  uart_puts("\n");
    uart_puts("IDI0 (0x108): 0x"); uart_hex(U1[U_IDI0/4]);
    uart_puts(" IPIPE(0x10C):0x"); uart_hex(U1[U_IPIPE/4]); uart_puts("\n");
    uart_puts("IBSA0(0x110): 0x"); uart_hex(U1[U_IBSA0/4]);
    uart_puts(" IBEA0(0x114):0x"); uart_hex(U1[U_IBEA0/4]); uart_puts("\n");
    uart_puts("IBLS (0x118): 0x"); uart_hex(U1[U_IBLS/4]);
    uart_puts(" IBWP(0x11C): 0x"); uart_hex(U1[U_IBWP/4]);  uart_puts("\n");
    uart_puts("MISC (0x400): 0x"); uart_hex(U1[U_MISC/4]);  uart_puts("\n");
    uart_puts("===================================\n");
}
#endif

/* ─── Complete Unicam1 initialization (matches bcm2835-unicam.c) ─────────── */
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
     * VPU leaves ANA=0x777 (APD=BPD=AR=DDL=1, CTAT=PTAT=7 = ALL POWERED DOWN).
     * Sequence: write 0x774 (hold AR, release APD/BPD/DDL) → wait → write 0x770
     * (release AR). This matches BCM2835 D-PHY power-on spec.
     */
    U_WRITE(U_ANA, 0x774u);   /* APD=0,BPD=0,AR=1 (hold reset), CTAT=7,PTAT=7 */
    delay_nop(200000);         /* ~1ms: wait for analog bandgap to settle */
    U_WRITE(U_ANA, 0x770u);   /* AR=0: release D-PHY analog reset */
    delay_nop(50000);          /* ~0.25ms: DDL lock time */

    /* STEP 3: Lane configuration (before CPR per Linux driver order)
     *
     * 0x1D = CLE|CLLPE|CLHSE|CLTRE — enables both LP and HS on the lane.
     * IMX708 uses CONTINUOUS HS clock mode, so CLK=0x1D is required.
     * DAT0=0x1D: data lane with full LP+HS termination.
     * DAT1=0x00: disabled (1-lane mode).
     * DAT2/3=0x00: disabled.
     *
     * Preserve upper 16 bits (hi-counter from VPU, informational only).
     */
    U_WRITE(U_CLK,  (U_READ(U_CLK)  & 0xFFFF0000u) | 0x1Du);
    U_WRITE(U_DAT0, (U_READ(U_DAT0) & 0xFFFF0000u) | 0x1Du);
    U_WRITE(U_DAT1, (U_READ(U_DAT1) & 0xFFFF0000u) | 0x00u);  /* 1-lane: disabled */
    U_WRITE(U_DAT2, 0x00u);
    U_WRITE(U_DAT3, 0x00u);

    /* STEP 4: Timing registers
     * CLT/DLT = 0x0602: CLT1/DLT1=2 (term_en), CLT2/DLT2=6 (settle)
     * Values from Linux bcm2835-unicam.c for IMX708 at 450MHz link rate.
     */
    U_WRITE(U_CLT, 0x0602u);
    U_WRITE(U_DLT, 0x0602u);

    /* STEP 5: CPR pulse (D-PHY reset — now safe because ANA is powered)
     * Must be SET then CLEARED (pulse, not level).
     */
    U_SETBITS(U_CTRL, U_CTRL_CPR);
    delay_nop(2000);               /* brief reset pulse */
    U_CLRBITS(U_CTRL, U_CTRL_CPR);
    delay_nop(2000);

    /* STEP 6: Full CTRL with PFT and OET timeouts
     * CTRL_BASE = MEM|PFT=0xF|OET=128 = 0x080F02
     * PFT (Packet Framer Timeout [15:8]=0xF): prevents framer from aborting
     * prematurely before FS packet completes reception.
     * OET (Output Enable Timeout [20:12]=128): DMA output timing.
     */
    U_WRITE(U_CTRL, U_CTRL_BASE);

    /* STEP 7: AXI bus priority (PE=1,PT=2,NP=8,PP=0xE) */
    U_WRITE(U_PRI, 0x00000E85u);

    /* STEP 8: Disable capture window (full frame DMA, no crop) */
    U_WRITE(U_IHWIN, 0x00u);
    U_WRITE(U_IVWIN, 0x00u);

    /* STEP 9: CMP0 — secondary Frame End detection via STA.PI0=BIT(15)
     * PCE=BIT(31), GI=BIT(9), CPH=BIT(8), VC=0, DT=0x01 (FE short pkt)
     * This provides an alternative FE detection path if ISTA_FEI is unreliable.
     */
    U_WRITE(U_CMP0, 0x80000301u);

    /* STEP 10: IDI0 — CSI-2 data type filter *** CRITICAL ***
     * Format: (VC<<6) | DT
     * RAW8 DT=0x2A, VC=0 → IDI0=0x2A
     * IDI0=0 means "accept Frame Start only" → pixel data IGNORED → ISTA_FEI never fires!
     */
    U_WRITE(U_IDI0, (0u << 6) | 0x2Au);   /* VC=0, DT=RAW8 */

    /* STEP 11: IPIPE — image pipe configuration *** CRITICAL ***
     * PUM=0 (no unpack), PPM=0 (no pack) = RAW8 native passthrough.
     * Note: IPIPE=0 was confirmed by Linux source for RAW8 native format.
     * (V40's hypothesis of IPIPE=0x80 was wrong; Linux uses 0x00 for passthrough)
     */
    U_WRITE(U_IPIPE, 0x00u);

    /* STEP 12: DMA buffer addresses
     * IBSA0: bus address with 0x40000000 L2-bypass alias (ARM AXI path)
     * IBEA0: exclusive end = start + total_bytes
     * IBLS:  line stride in bytes
     */
    const uint32_t phys_addr = (uint32_t)(uintptr_t)g_raw_frame;
    const uint32_t bus_addr  = 0x40000000u | phys_addr;   /* L2-bypass */
    U_WRITE(U_IBSA0, bus_addr);
    U_WRITE(U_IBEA0, bus_addr + FRAME_SZ);
    U_WRITE(U_IBLS,  FRAME_W);              /* bytes per line */

    /* STEP 13: MISC — frame limit bits FL0=BIT(6), FL1=BIT(9)
     * Read-modify-write: set both bits (value = 0x240)
     */
    U_SETBITS(U_MISC, (1u << 6) | (1u << 9));

    /* STEP 14: Clear status registers before enabling interrupts */
    U_WRITE(U_STA,  0xFFFFFFFFu);
    U_WRITE(U_ISTA, 0x00000007u);    /* write 1 to FSI|FEI|LCI to clear */

    /* STEP 15: ICTL — interrupt enables (before CPE per Linux) */
    U_WRITE(U_ICTL, U_ICTL_FSIE | U_ICTL_FEIE | U_ICTL_IBOB);  /* = 0x07 */

    /* STEP 16: CPE — enable the peripheral */
    U_SETBITS(U_CTRL, U_CTRL_CPE);   /* CTRL = CTRL_BASE | CPE = 0x080F03 */

    /* STEP 17: MISC again after CPE (Linux re-asserts FL0|FL1 after CPE) */
    U_SETBITS(U_MISC, (1u << 6) | (1u << 9));

    /* STEP 18: LIP — Load Image Pointers (MUST be AFTER CPE)
     * LIP is GENMASK(6:5) in ICTL. Writing value 1 sets BIT(5).
     * Self-clearing: ICTL reads back as 0x07 after LIP completes.
     * LIP latches IBSA0/IBEA0 into the active DMA address registers.
     */
    U_SETBITS(U_ICTL, U_ICTL_LIP);   /* ICTL = 0x07 | 0x20 = 0x27 */

    uart_puts("[UNICAM] V76 init complete. CTRL=0x");
    uart_hex(U_READ(U_CTRL));
    uart_puts(" IDI0=0x");
    uart_hex(U_READ(U_IDI0));
    uart_puts(" IPIPE=0x");
    uart_hex(U_READ(U_IPIPE));
    uart_puts(" IBSA0=0x");
    uart_hex(U_READ(U_IBSA0));
    uart_puts("\n");
    uart_puts("[UNICAM] IBEA0=0x");
    uart_hex(U_READ(U_IBEA0));
    uart_puts(" MISC=0x");
    uart_hex(U_READ(U_MISC));
    uart_puts(" ICTL=0x");
    uart_hex(U_READ(U_ICTL));
    uart_puts("\n");
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

void unicam_init() {
    /* Enable Unicam1 clock domain FIRST — before any register access */
    *UNICAM1_CLKGATE = CM_PASSWD | 0x05u;  /* CLK+DAT0 gates for 1-lane */
#ifdef SIMULATION
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

    if (!g_sim_state.clkgate_enabled) {
        uart_puts("[SIM] FAIL: CLKGATE not written — Unicam1 clock domain GATED OFF\n");
        ok = false;
    }
    if (!g_sim_state.mem_bit_set) {
        uart_puts("[SIM] FAIL: CTRL.MEM never set — DMA interface disabled\n");
        ok = false;
    }
    if (!g_sim_state.ana_powered_up) {
        uart_puts("[SIM] FAIL: ANA=0x777 (D-PHY fully powered down) — CSI-2 RX blind\n");
        ok = false;
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
        ok = false;
    }
    if (!g_sim_state.dat0_lane_enabled) {
        uart_puts("[SIM] FAIL: DAT0 lane lower bits = 0 — data lane disabled\n");
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
    uart_puts("[SIM]   IDI0=0x"); uart_hex(g_sim_state.reg_idi0);
    uart_puts("  IPIPE=0x");      uart_hex(g_sim_state.reg_ipipe);
    uart_puts("  IBSA0=0x");      uart_hex(g_sim_state.reg_ibsa0);
    uart_puts("  IBEA0=0x");      uart_hex(g_sim_state.reg_ibea0);
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
    for (unsigned long i = 0; i < 45000000UL; i++) {
        if (i % 100000 == 0) watchdog_kick();

        uint32_t ista = U1[U_ISTA/4];
        uint32_t sta  = U1[U_STA/4];

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
    uart_puts("[UNICAM] Diagnostic:\n");
    uart_puts("  IBWP=0x"); uart_hex(U1[U_IBWP/4]);
    uart_puts("  (if == IBSA0, sensor not transmitting or IDI0 wrong)\n");
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
    uart_puts(" WP=0x");  uart_hex(U_READ(U_IBWP));
    uart_puts("\n");
}

const uint8_t* unicam_frame_ptr() { return g_raw_frame; }
int unicam_frame_w() { return FRAME_W; }
int unicam_frame_h() { return FRAME_H; }
