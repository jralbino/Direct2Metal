#include "imx708.h"
#include "imx708_regs.h"
#include "bsc.h"
#include "uart.h"
#include <stdint.h>

#define IMX708_ADDR 0x1A

void imx708_write(uint16_t reg, uint8_t val) {
    uint8_t data[3];
    data[0] = reg >> 8;
    data[1] = reg & 0xFF;
    data[2] = val;
    bsc_write(IMX708_ADDR, data, 3);
}

uint8_t imx708_read(uint16_t reg) {
    uint8_t addr[2];
    addr[0] = reg >> 8;
    addr[1] = reg & 0xFF;
    if (!bsc_write(IMX708_ADDR, addr, 2)) return 0xFF;
    uint8_t val = 0;
    if (!bsc_read(IMX708_ADDR, &val, 1)) return 0xFF;
    return val;
}

bool imx708_probe() {
    for (int attempt = 0; attempt < 4; attempt++) {
        uint8_t id_high = imx708_read(0x0016);
        uint8_t id_low  = imx708_read(0x0017);
        uint16_t chip_id = (id_high << 8) | id_low;
        if (chip_id == 0x0708) {
            uart_puts("IMX708: probe OK (ID=0x0708)\n");
            return true;
        }
        for (volatile int i = 0; i < 100000; i++);
    }
    uart_puts("IMX708: probe FAILED\n");
    return false;
}

uint8_t imx708_read_frame_count() {
    return imx708_read(0x0005);
}

void imx708_init_baseline() {
    /* V159: binned 1536×864 mode. Two-stage init mirrors the official
     * raspberrypi/linux imx708.c driver: common regs first, then mode regs. */
    for (int i = 0; i < k_imx708_common_len; i++)
        imx708_write(k_imx708_common[i].reg, k_imx708_common[i].val);

    /* On-die lens-shading correction LUT — Linux applies it between common and
     * binned blocks (linux_extract trace lines 50-157). 108 register writes. */
    for (int i = 0; i < k_imx708_lsc_len; i++)
        imx708_write(k_imx708_lsc[i].reg, k_imx708_lsc[i].val);

    for (int i = 0; i < k_imx708_binned_len; i++)
        imx708_write(k_imx708_binned[i].reg, k_imx708_binned[i].val);

    /* Stay in standby until stream_on */
    imx708_write(0x0100, 0x00);
}

void imx708_stream_on() {
    imx708_write(0x0100, 0x01);
}
