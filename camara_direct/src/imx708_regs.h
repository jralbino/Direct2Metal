/* IMX708 register table — BINNED MODE 1536×864 RAW10.
 *
 * V159: switched from full mode (4608×2592) to binned mode for higher fps.
 * Sensor reads a 3072×1728 centered crop from the 4608×2592 array
 * (x_addr=768..3839, y_addr=432..2159) and applies 2×2 binning to produce
 * 1536×864 output. Bayer pattern stays BGGR after the 0x0101=0x03 H+V flip.
 *
 * Tables sourced from the parent project's working binned 1536x864 setup
 * (Direct2Metal/src/imx708_regs.h k_imx708_common[] + k_imx708_init[])
 * which is itself a port of the official raspberrypi/linux
 * imx708.c mode_2x2binned_720p_regs[] sequence with these overrides
 * proven on hardware:
 *   - 0x0310=0x00 non-continuous HS clock (V104 HW-verified, official is 0x01)
 *   - 0x0204/05=0x03C0 ANA_GAIN=16x (V122; libcamera default 0x0070=1.12x is too dim)
 *   - 0x020E/0F=0x0200 DIG_GAIN=2x (V122)
 *   - 0x0220=0x00 + secondary-exposure regs zeroed (V150, HDR/DOL fully OFF)
 *   - 0x0B8E/0B94=0x00 PDAF disabled (V149)
 *   - 0x3400=0x00 embedded-data disabled (V149) — Unicam has no IDI1 metadata route
 *   - CIT 0x0202/03=0x046B (V134 anti-flicker, ~36.8 µs × 1131 lines)
 *
 * Sensor stays in standby (0x0100=0x00) until imx708_stream_on() asserts
 * 0x0100=0x01. */
#pragma once

#include <stdint.h>

struct RegVal { uint16_t reg; uint8_t val; };

/* Common register block — applied first in init. Contains: standby, EXCK_FREQ,
 * undocumented internal calibration, CSI-2 PHY/lane config, RAW10 format,
 * 2-lane MIPI, PDAF/embedded disables, late-write internal calibration. */
static const RegVal k_imx708_common[] = {
    /* standby first */
    { 0x0100, 0x00 },
    /* EXCK_FREQ = 24.0 MHz */
    { 0x0136, 0x18 }, { 0x0137, 0x00 },
    /* undocumented internal calibration */
    { 0x33F0, 0x02 }, { 0x33F1, 0x05 },
    /* CSI-2 internal PHY / lane config */
    { 0x3062, 0x00 }, { 0x3063, 0x12 },
    { 0x3068, 0x00 }, { 0x3069, 0x12 },
    { 0x306A, 0x00 }, { 0x306B, 0x30 },
    { 0x3076, 0x00 }, { 0x3077, 0x30 },
    { 0x3078, 0x00 }, { 0x3079, 0x30 },
    /* more undocumented internal calibration */
    { 0x5E54, 0x0C },
    { 0x6E44, 0x00 },
    { 0xB0B6, 0x01 },
    { 0xE829, 0x00 },
    { 0xF001, 0x08 }, { 0xF003, 0x08 },
    { 0xF00D, 0x10 }, { 0xF00F, 0x10 },
    { 0xF031, 0x08 }, { 0xF033, 0x08 },
    { 0xF03D, 0x10 }, { 0xF03F, 0x10 },
    /* CSI-2 data format: RAW10 */
    { 0x0112, 0x0A }, { 0x0113, 0x0A },
    /* 2-lane MIPI */
    { 0x0114, 0x01 },
    /* PDAF disabled (V149) */
    { 0x0B8E, 0x00 }, { 0x0B8F, 0x00 },
    { 0x0B94, 0x00 }, { 0x0B95, 0x00 },
    /* embedded data disabled (V149) — DT=0x12 packets bleed into image buffer
     * because Unicam has no metadata routing here */
    { 0x3400, 0x00 },
    { 0x3478, 0x01 }, { 0x3479, 0x1C },
    { 0x3091, 0x01 }, { 0x3092, 0x00 },
    { 0x3419, 0x00 },
    { 0xBCF1, 0x02 },
    { 0x3094, 0x01 }, { 0x3095, 0x01 },
    { 0x3362, 0x00 }, { 0x3363, 0x00 },
    { 0x3364, 0x00 }, { 0x3365, 0x00 },
    { 0x0138, 0x01 },
    /* MIPI OP-PLL multiplier for 450 MHz DDR */
    { 0x030E, 0x01 }, { 0x030F, 0x2C },
};
static const int k_imx708_common_len = sizeof(k_imx708_common)/sizeof(RegVal);

/* Mode-specific block — applied AFTER common. Configures the binned
 * 1536×864 readout: line/frame length, analog crop window, binning,
 * DIG_CROP, PLL, exposure/gain, HDR-off. */
static const RegVal k_imx708_binned[] = {
    /* Image orientation: H+V flip — sensor output becomes BGGR */
    { 0x0101, 0x03 },
    /* Line length pck = 0x1460 = 5216 (binned mode) */
    { 0x0342, 0x14 }, { 0x0343, 0x60 },
    /* Frame length lines = 0x046D = 1133 → ~24 fps at this PLL config */
    { 0x0340, 0x04 }, { 0x0341, 0x6D },
    /* Analog readout window: x=768..3839 (3072 wide), y=432..2159 (1728 tall).
     * 2x2 binning of this 3072×1728 crop produces 1536×864 output. */
    { 0x0344, 0x03 }, { 0x0345, 0x00 },
    { 0x0346, 0x01 }, { 0x0347, 0xB0 },
    { 0x0348, 0x0E }, { 0x0349, 0xFF },
    { 0x034A, 0x08 }, { 0x034B, 0x6F },
    /* HDR/DOL fully OFF (V150) — linear single-exposure readout */
    { 0x0220, 0x00 }, { 0x0222, 0x01 },
    /* 2x2 binning enabled: 0x0900=0x01, factors 0x0901=0x22 (h=2 v=2),
     * weight 0x0902=0x08 (per official kernel sequence) */
    { 0x0900, 0x01 }, { 0x0901, 0x22 }, { 0x0902, 0x08 },
    { 0x3200, 0x41 }, { 0x3201, 0x41 },
    { 0x32D5, 0x00 }, { 0x32D6, 0x00 },
    { 0x32DB, 0x01 }, { 0x32DF, 0x01 },
    { 0x350C, 0x00 }, { 0x350D, 0x00 },
    /* DIG_CROP: offset 0,0 size 1536×864 (full passthrough of binned image) */
    { 0x0408, 0x00 }, { 0x0409, 0x00 },
    { 0x040A, 0x00 }, { 0x040B, 0x00 },
    { 0x040C, 0x06 }, { 0x040D, 0x00 },
    { 0x040E, 0x03 }, { 0x040F, 0x60 },
    /* x/y output size = 1536 × 864 */
    { 0x034C, 0x06 }, { 0x034D, 0x00 },
    { 0x034E, 0x03 }, { 0x034F, 0x60 },
    /* PLL chain (binned mode: 0x0307=0x76 vs full's 0x7C) */
    { 0x0301, 0x05 }, { 0x0303, 0x02 },
    { 0x0305, 0x02 }, { 0x0306, 0x00 },
    { 0x0307, 0x76 },
    { 0x030B, 0x02 }, { 0x030D, 0x04 },
    /* V104 override: 0x0310=0x00 non-continuous HS clock (HW-verified) */
    { 0x0310, 0x00 },
    /* 3CA0-3CBF timing block (per official binned sequence) */
    { 0x3CA0, 0x00 }, { 0x3CA1, 0x3C },
    { 0x3CA4, 0x01 }, { 0x3CA5, 0x5E },
    { 0x3CA6, 0x00 }, { 0x3CA7, 0x00 },
    { 0x3CAA, 0x00 }, { 0x3CAB, 0x00 },
    { 0x3CB8, 0x00 }, { 0x3CB9, 0x0C },
    { 0x3CBA, 0x00 }, { 0x3CBB, 0x04 },
    { 0x3CBC, 0x00 }, { 0x3CBD, 0x1E },
    { 0x3CBE, 0x00 }, { 0x3CBF, 0x05 },
    /* Coarse integration time = 0x046B = 1131 lines (V134 anti-flicker;
     * line time at binned config ≈ 36.8 µs → ~41.6 ms exposure) */
    { 0x0202, 0x04 }, { 0x0203, 0x6B },
    /* secondary integration zeroed (HDR off → no short exposure) */
    { 0x0224, 0x00 }, { 0x0225, 0x00 },
    { 0x3116, 0x00 }, { 0x3117, 0x00 },
    /* Primary analog gain = 0x03C0 = 16x (V122 override) */
    { 0x0204, 0x03 }, { 0x0205, 0xC0 },
    /* secondary analog gain zeroed */
    { 0x0216, 0x00 }, { 0x0217, 0x00 },
    /* secondary coarse integration zeroed */
    { 0x0218, 0x00 }, { 0x0219, 0x00 },
    /* Primary digital gain = 0x0200 = 2x (V122 override) */
    { 0x020E, 0x02 }, { 0x020F, 0x00 },
    /* secondary digital gain + secondary integration zeroed */
    { 0x3118, 0x00 }, { 0x3119, 0x00 },
    { 0x311A, 0x00 }, { 0x311B, 0x00 },
    /* HDR control range fully zeroed (V150) */
    { 0x341A, 0x00 }, { 0x341B, 0x00 },
    { 0x341C, 0x00 }, { 0x341D, 0x00 },
    { 0x341E, 0x00 }, { 0x341F, 0x00 },
    { 0x3420, 0x00 }, { 0x3421, 0x00 },
    { 0x3366, 0x00 }, { 0x3367, 0x00 },
    { 0x3368, 0x00 }, { 0x3369, 0x00 },
};
static const int k_imx708_binned_len = sizeof(k_imx708_binned)/sizeof(RegVal);
