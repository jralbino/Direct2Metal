/* File: src/camera_imx708.cpp
 * V76 — IMX708 Sensor Initialization
 *
 * ── LANE MODE DECISION ───────────────────────────────────────────────────────
 * V73 tested 2-lane (0x0114=0x01): D0hi=D1hi=51712 constantly — sensor SILENT.
 * Physical D1 on Pi Zero 2W CSI connector may have impedance issues at 450Mbps.
 * V72 tested 1-lane (0x0114=0x00): D0hi oscillates HS↔LP-11 — sensor TRANSMITS.
 *
 * Reverted to 1-lane (0x0114=0x00 override) until 2-lane physical issue is
 * diagnosed. Sensor transmits valid MIPI on D0 in 1-lane mode.
 *
 * NOTE: Linux rejects 1-lane at driver level, but the sensor hardware supports
 * it and demonstrates MIPI transmission. Once Unicam decoder is working, we
 * can test 2-lane after verifying physical D1 connectivity.
 */
#include "hardware_sim.h"
#include <stdint.h>
#define IMX708_ADDR  0x1Au

extern void    bsc_init_and_find();
extern void    i2c_write_reg16(uint8_t dev_addr, uint16_t reg, uint8_t val);
extern uint8_t i2c_read_reg16 (uint8_t dev_addr, uint16_t reg);
extern void uart_puts(const char* s);
extern void uart_dec(int n);

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
    g_sim_state.sensor_lane_count = 1;   /* 1-lane mode for V76 */
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

    /* V89: TEST 2-LANE mode — re-evaluate V73 "2-lane SILENT" conclusion.
     *
     * V73 tested 2-lane but had the CPR bug: CLK/DAT0 configured BEFORE CPR,
     * then CPR wiped them to 0x02 (power-down). With CLK lane in power-down,
     * Unicam never asserted 100Ω termination on CLK or D1. The sensor sees
     * missing termination on D1 → keeps both lanes in LP-11 → "SILENT".
     *
     * V73 diagnosis ("sensor SILENT") was actually the CPR bug, not a physical
     * D1 connectivity problem. With V81 CPR fix + DAT1=0x1D (termination on D1),
     * the sensor in 2-lane mode should now see proper 100Ω on both D0 and D1
     * and transition to HS on both lanes.
     *
     * k_imx708_common already sets 0x0114=0x01 (2-lane). We no longer override.
     * If 2-lane works (STA fires): V73's "silent" was CPR bug, D1 IS connected.
     * If 2-lane also fails: decoder issue independent of lane count.
     */
    i2c_write_reg16(IMX708_ADDR, 0x0112, 0x08);  /* RAW8 MSB */
    i2c_write_reg16(IMX708_ADDR, 0x0113, 0x08);  /* RAW8 LSB */
    i2c_write_reg16(IMX708_ADDR, 0x0114, 0x01);  /* V89: 2-lane test (k_imx708_common default) */
    i2c_write_reg16(IMX708_ADDR, 0x0100, 0x00);  /* standby until stream_on() */

    uint8_t lane_mode = i2c_read_reg16(IMX708_ADDR, 0x0114);
    uint8_t pixel_fmt = i2c_read_reg16(IMX708_ADDR, 0x0112);
    uart_puts("[IMX708] V89: 0x0114="); uart_dec((int)lane_mode);
    uart_puts(" (0=1lane,1=2lane)  0x0112="); uart_dec((int)pixel_fmt);
    uart_puts(" (8=RAW8)\n");
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
