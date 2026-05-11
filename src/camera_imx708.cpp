/* File: src/camera_imx708.cpp
 * V104 — IMX708 Sensor Initialization
 *
 * ── LANE MODE HISTORY ────────────────────────────────────────────────────────
 * V72: 1-lane (0x0114=0x00) — D0hi oscillates HS↔LP-11 (sensor transmits).
 *      But had CPR bug (CLK wiped) → Unicam CLK lane in power-down → STA=0.
 * V73: 2-lane test — D1hi flat (sensor silent on D1 due to CPR bug/no term).
 * V89: 2-lane with CPR fixed + ANA=0x770 + settle=6 — D1hi=57344 (HS on D1).
 *      CLKGATE=0x02, IDI0=0x2A(RAW8-WRONG), STA=0. D1 physically confirmed OK.
 * V93: 2-lane with all fixes (CPM=0, CLKGATE=0x5A000015, IDI0=0x2B, IBLS=1920).
 *      D-PHY shows HS on CLK+D0+D1. STA=0 persists → CSI-2 byte sync failing.
 * V103: Matched Pi OS register dump exactly (CLK=0x0005, 2-lane, 100MHz, etc.).
 *       STA=0 persists. Sensor init suspect: 0x0310=0x01 sets continuous HS clock.
 *
 * ── V104: MIPI CLOCK MODE FIX ────────────────────────────────────────────────
 * k_imx708_init had 0x0310=0x01 (continuous HS clock mode).
 * BCM2835-unicam uses use_lp_clock=true for IMX708 → CLK=0x0005 (no CLHSE/CLTRE).
 * use_lp_clock=true requires sensor in NON-CONTINUOUS mode (CLK lane LP-11 idle).
 * With continuous HS clock (0x0310=0x01) + CLK=0x0005: HS receiver not enabled
 * on CLK lane → byte clock not recovered → CSI-2 decoder never syncs → STA=0.
 * Fix: 0x0310=0x00 (non-continuous HS clock — CLK lane LP-11 between frames).
 */
#include "hardware_sim.h"
#include <stdint.h>
#define IMX708_ADDR  0x1Au

extern void    bsc_init_and_find();
extern void    i2c_write_reg16(uint8_t dev_addr, uint16_t reg, uint8_t val);
extern uint8_t i2c_read_reg16 (uint8_t dev_addr, uint16_t reg);
extern void uart_puts(const char* s);
extern void uart_dec(int n);
extern void uart_hex(uint32_t n);

struct RegVal { uint16_t reg; uint8_t val; };
#include "imx708_regs.h"

bool imx708_probe() {
    uint8_t id_hi = i2c_read_reg16(IMX708_ADDR, 0x0016);
    uint8_t id_lo = i2c_read_reg16(IMX708_ADDR, 0x0017);
    return (id_hi == 0x07 && id_lo == 0x08);
}

bool imx708_init() {
#ifdef SIMULATION
    uart_puts("[SIM] Bypass I2C: IMX708 simulated present (ID=0x0708)\n");
    g_sim_state.sensor_lane_count = 2;   /* V104: 2-lane, non-continuous HS clock */
    return true;
#endif

    bsc_init_and_find();
    if (!imx708_probe()) {
        uart_puts("[IMX708] Probe FAILED — sensor not found\n");
        return false;
    }
    uart_puts("[IMX708] OK (0x0708) 1536x864 2lane RAW10\n");

    for (int i = 0; i < k_imx708_common_len; i++)
        i2c_write_reg16(IMX708_ADDR, k_imx708_common[i].reg, k_imx708_common[i].val);

    /* V162 port: lens-shading correction LUT, applied between common and
     * mode blocks (matches libcamera ordering). Compensates for vignetting
     * in the IMX708 — corners are dim without these writes. */
    for (int i = 0; i < k_imx708_lsc_len; i++)
        i2c_write_reg16(IMX708_ADDR, k_imx708_lsc[i].reg, k_imx708_lsc[i].val);

    /* V150: SPC PDAF gain writes still skipped (PDAF disabled in common regs). */
    uart_puts("[IMX708] V150 HDR/DOL+PDAF disabled — skipping SPC gain writes\n");

    for (int i = 0; i < k_imx708_init_len; i++)
        i2c_write_reg16(IMX708_ADDR, k_imx708_init[i].reg, k_imx708_init[i].val);

    /* V148: TEST PATTERN DISABLED — real scene capture.
     * V147 SOLID test proved Unicam/CSI-2/DMA transport is clean (every row
     * matched expected bytes perfectly, no banding or shift). The 960-byte cyclic
     * shift seen in V145 color bars was intrinsic to the IMX708 test pattern
     * generator + 2x2 binning pipeline, not a transport bug. */
    i2c_write_reg16(IMX708_ADDR, 0x0600, 0x00);
    i2c_write_reg16(IMX708_ADDR, 0x0601, 0x00);
    uart_puts("[IMX708] V150 HDR/DOL+PDAF+embedded OFF, test pattern OFF (real scene)\n");

    i2c_write_reg16(IMX708_ADDR, 0x0100, 0x00);  /* standby until stream_on() */
    return true;
}

void imx708_stream_on() {
#ifdef SIMULATION
    g_sim_state.sensor_streaming = true;
    uart_puts("[SIM] Sensor stream_on: simulated MIPI transmission started\n");
    return;
#endif
    i2c_write_reg16(IMX708_ADDR, 0x0100, 0x01);
}

uint8_t imx708_read_frame_count() {
#ifdef SIMULATION
    return (uint8_t)(g_sim_state.frame_number * 3 + 10);  /* synthetic count */
#endif
    return i2c_read_reg16(IMX708_ADDR, 0x0005);
}

void imx708_stream_off() {
#ifdef SIMULATION
    g_sim_state.sensor_streaming = false;
    return;
#endif
    i2c_write_reg16(IMX708_ADDR, 0x0100, 0x00);
}
