/* File: src/camera_imx708.cpp
 * V115 — IMX708 Sensor Initialization
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
    g_sim_state.sensor_test_pattern = 0; /* V113: test pattern off (V112 confirmed data path) */
    return true;
#endif

    bsc_init_and_find();
    if (!imx708_probe()) {
        uart_puts("[IMX708] Probe FAILED — sensor not found\n");
        return false;
    }
    uart_puts("[IMX708] Probe OK (ID=0x0708)\n");

    uart_puts("[IMX708] Writing common registers...\n");
    for (int i = 0; i < k_imx708_common_len; i++) {
        i2c_write_reg16(IMX708_ADDR, k_imx708_common[i].reg, k_imx708_common[i].val);
    }

    uart_puts("[IMX708] Writing mode registers (1536x864 binned)...\n");
    for (int i = 0; i < k_imx708_init_len; i++) {
        i2c_write_reg16(IMX708_ADDR, k_imx708_init[i].reg, k_imx708_init[i].val);
    }

    /* V91: Use native RAW10 — k_imx708_common already sets 0x0114=0x01 (2-lane)
     * and 0x0112=0x0A (RAW10). IDI0=0x2B, IBLS=1920 match this format. */

    /* V113: Test pattern disabled — V112 confirmed data path works (color bars OK).
     * Dark image = H7 (optical obstruction). Keeping test pattern OFF for real capture.
     * To re-enable: 0x0600=0x00, 0x0601=0x02 (color bars). */
    i2c_write_reg16(IMX708_ADDR, 0x0600, 0x00);  /* high byte = 0 */
    i2c_write_reg16(IMX708_ADDR, 0x0601, 0x00);  /* low byte = 0 = off */

    /* V112: Boost analog gain to maximum.
     * 0x0204/0x0205 = ANALOG_GAIN. Was 0x0070 (1.12x). Now 0x03C0 (16x).
     * Gain = 1024/(1024-960) = 16x. Will amplify any real signal significantly.
     * (Test pattern ignores gain, so this only matters for future real captures.) */
    i2c_write_reg16(IMX708_ADDR, 0x0204, 0x03);  /* gain high byte */
    i2c_write_reg16(IMX708_ADDR, 0x0205, 0xC0);  /* gain low byte */

    /* V103: Restore 2-lane mode — Pi OS uses 2-lane (0x0114=0x01).
     * k_imx708_common sets 0x0114=0x01 (2-lane) — no override needed.
     * V94 forced 1-lane (0x0114=0x00) as diagnostic — falsified, reverting.
     * Unicam side: DAT1=0x05 (DLE|DLLPE), matching Pi OS exactly. */
    i2c_write_reg16(IMX708_ADDR, 0x0100, 0x00);  /* standby until stream_on() */

    uint8_t lane_mode    = i2c_read_reg16(IMX708_ADDR, 0x0114);
    uint8_t pixel_fmt    = i2c_read_reg16(IMX708_ADDR, 0x0112);
    uart_puts("[IMX708] V113: 0x0114="); uart_dec((int)lane_mode);
    uart_puts(" (1=2lane)  0x0112="); uart_dec((int)pixel_fmt);
    uart_puts(" (10=RAW10)\n");

    /* V113: Readback gain + test pattern to confirm I2C writes */
    {
        uint8_t tp_hi      = i2c_read_reg16(IMX708_ADDR, 0x0600);
        uint8_t tp_lo      = i2c_read_reg16(IMX708_ADDR, 0x0601);
        uint8_t gain_hi    = i2c_read_reg16(IMX708_ADDR, 0x0204);
        uint8_t gain_lo    = i2c_read_reg16(IMX708_ADDR, 0x0205);
        uint8_t clk_mode   = i2c_read_reg16(IMX708_ADDR, 0x0310);
        uart_puts("[IMX708] V113: test_pattern=");
        uart_dec((int)((tp_hi << 8) | tp_lo));
        uart_puts(" (expect 0=off)  gain=");
        uart_hex(((uint32_t)gain_hi << 8) | gain_lo);
        uart_puts(" (expect 0x03C0=16x)\n");
        uart_puts("[IMX708] V113: 0x0310(clk_mode)="); uart_dec((int)clk_mode);
        uart_puts(" (expect 0=non-continuous)\n");
    }
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
