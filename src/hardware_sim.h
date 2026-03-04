/* File: src/hardware_sim.h
 * V76 — BCM2837 Unicam1 Hardware Simulator
 *
 * Provides a register-level software emulation of the BCM2837 Unicam1
 * CSI-2 receiver. In SIMULATION mode (-DSIMULATION), all MMIO reads/writes
 * are intercepted and validated against the correct Linux driver sequence.
 *
 * The simulator acts as a "verification oracle": if the register sequence
 * matches bcm2835-unicam.c unicam_start_rx(), ISTA_FE is synthesized and
 * capture succeeds. Otherwise, it reports EXACTLY which step was wrong.
 */
#ifndef HARDWARE_SIM_H
#define HARDWARE_SIM_H

#include <stdint.h>
#include <stdbool.h>

/* ─── BCM2837 Unicam1 Register Offsets (confirmed from vc4-regs-unicam.h) ── */
/* Peripheral control block (0x000-0x03C) */
#define U_CTRL   0x000
#define U_STA    0x004
#define U_ANA    0x008
#define U_PRI    0x00C   /* AXI priority */
#define U_CLK    0x010   /* Clock lane config */
#define U_CLT    0x014   /* Clock lane timing: CLT1[7:0]=term, CLT2[15:8]=settle */
#define U_DAT0   0x018   /* Data lane 0 config */
#define U_DAT1   0x01C   /* Data lane 1 config */
#define U_DAT2   0x020   /* Data lane 2 config */
#define U_DAT3   0x024   /* Data lane 3 config */
#define U_DLT    0x028   /* Data lane timing: DLT1[7:0]=term, DLT2[15:8]=settle */
#define U_CMP0   0x02C   /* Compare register 0 (fires STA.PI0 on FE) */
#define U_CMP1   0x030
#define U_CAP0   0x034
#define U_CAP1   0x038

/* Image capture block (0x100-0x14C) */
#define U_ICTL   0x100   /* Image control: FSIE[0],FEIE[1],IBOB[2],LIP[6:5] */
#define U_ISTA   0x104   /* Image status: FSI[0],FEI[1] (write 1 to clear) */
#define U_IDI0   0x108   /* Image data ID: (VC<<6)|DT  (RAW8=0x2A) *** CRITICAL *** */
#define U_IPIPE  0x10C   /* Image pipe: PUM[2:0], PPM[9:7] *** CRITICAL *** */
#define U_IBSA0  0x110   /* Image buffer A start address */
#define U_IBEA0  0x114   /* Image buffer A end address *** CRITICAL (was missing) *** */
#define U_IBLS   0x118   /* Image buffer line stride (bytes per line) */
#define U_IBWP   0x11C   /* Image buffer write pointer (read-only diagnostic) */
#define U_IHWIN  0x120   /* Horizontal window (0=disabled) */
#define U_IVWIN  0x128   /* Vertical window (0=disabled) */

/* Double-buffer section (single-buffer: do NOT write these) */
#define U_IBSA1  0x304
#define U_IBEA1  0x308

/* Misc (0x400) */
#define U_MISC   0x400   /* FL0=BIT(6), FL1=BIT(9) = 0x240 */

/* ─── CTRL Register Bit Fields ───────────────────────────────────────────── */
#define U_CTRL_CPE   (1u << 0)      /* Control Peripheral Enable */
#define U_CTRL_MEM   (1u << 1)      /* Memory/DMA Interface Enable */
#define U_CTRL_CPR   (1u << 2)      /* Control Peripheral Reset */
/* bits [15:8] = PFT_MASK (Packet Framer Timeout) */
/* bits [20:12] = OET_MASK (Output Enable Timeout) */
/* NOTE: BIT(14) is NOT "LSM" — it is part of OET_MASK! */

/* Combined CTRL value from Linux bcm2835-unicam.c for CSI-2:
 * MEM=BIT(1) | PFT=0xF<<8 | OET=128<<12 = 0x080F02
 * With CPE: 0x080F03 */
#define U_CTRL_BASE  0x00080F02u

/* ─── ICTL Register Bit Fields ───────────────────────────────────────────── */
#define U_ICTL_FSIE  (1u << 0)   /* Frame Start Interrupt Enable */
#define U_ICTL_FEIE  (1u << 1)   /* Frame End Interrupt Enable */
#define U_ICTL_IBOB  (1u << 2)   /* Image Buffer Overflow Bounce */
/* LIP is GENMASK(6:5) — write 1 to bits[6:5] = BIT(5) = 0x20 */
#define U_ICTL_LIP   (1u << 5)   /* Load Image Pointers (self-clearing) */

/* ─── ISTA Register Bit Fields ───────────────────────────────────────────── */
#define U_ISTA_FSI   (1u << 0)   /* Frame Start Interrupt */
#define U_ISTA_FEI   (1u << 1)   /* Frame End Interrupt */

/* ─── STA Register Bit Fields ────────────────────────────────────────────── */
#define U_STA_PI0    (1u << 15)  /* CMP0 match — secondary FE detection */

/* ─── ANA Register Bit Fields ────────────────────────────────────────────── */
/* VPU leaves ANA=0x777 at boot (all bits powered down/reset) */
#define U_ANA_ALL_OFF 0x777u     /* APD=BIT(0),BPD=BIT(1),AR=BIT(2),DDL=BIT(3),
                                    CTATADJ[7:4]=7, PTATADJ[11:8]=7 */

/* ─── Simulator State Machine ─────────────────────────────────────────────── */
/* Tracks every hardware precondition in correct order.
 * If any step is missing/wrong, captures the first error as a string. */
struct UnicamSimState {
    /* Clocking */
    bool clkgate_enabled;       /* CLKGATE at 0x3F802004 written with password */

    /* D-PHY power sequence */
    bool mem_bit_set;           /* CTRL: MEM=1 */
    bool ana_powered_up;        /* ANA written to < 0x777 (analog ON) */
    bool ana_before_cpr;        /* ANA was powered BEFORE CPR — ordering OK */
    bool cpr_before_ana;        /* ANA was STILL OFF when CPR issued — ordering ERROR */
    bool cpr_pulsed;            /* CPR: set then cleared */

    /* Lane configuration */
    bool clk_lane_enabled;      /* CLK lower bits set (non-zero) */
    bool dat0_lane_enabled;     /* DAT0 lower bits set (non-zero) */

    /* Timing */
    bool clt_set;               /* CLT timing register written */
    bool dlt_set;               /* DLT timing register written */

    /* CTRL with PFT/OET timeouts */
    bool ctrl_pft_set;          /* PFT field non-zero in CTRL */

    /* Image capture pipeline — MOST CRITICAL */
    bool idi0_set;              /* IDI0 written to non-zero DT value */
    bool ipipe_configured;      /* IPIPE register touched (value noted) */
    bool ibsa0_set;             /* IBSA0 (DMA start address) written */
    bool ibea0_set;             /* IBEA0 (DMA end address) written *** */
    bool misc_fl_set;           /* MISC FL0|FL1 = 0x240 written */
    bool ictl_set;              /* ICTL interrupt enables written */
    bool cpe_enabled;           /* CPE bit in CTRL set */
    bool lip_triggered;         /* LIP bit in ICTL triggered after CPE */

    /* Sensor state */
    bool sensor_streaming;      /* sensor stream_on() called */
    uint8_t sensor_lane_count;  /* from I2C reg 0x0114: 1 or 2 */

    /* Error tracking */
    const char* error_msg;      /* First validation failure (NULL if OK) */

    /* Simulation frame counter */
    int frame_number;

    /* Shadow register values for diagnostics */
    uint32_t reg_idi0;
    uint32_t reg_ipipe;
    uint32_t reg_ibsa0;
    uint32_t reg_ibea0;
    uint32_t reg_misc;
    uint32_t reg_ctrl;
    uint32_t reg_ana;
    uint32_t reg_clkgate;
};

/* Global simulation state — accessible by all camera modules */
extern UnicamSimState g_sim_state;

#endif /* HARDWARE_SIM_H */
