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
    uart_puts("IMX708: Probing sensor on I2C1...\n");
    for (int attempt = 0; attempt < 4; attempt++) {
        uint8_t id_high = imx708_read(0x0016);
        uint8_t id_low  = imx708_read(0x0017);
        uint16_t chip_id = (id_high << 8) | id_low;
        uart_puts("IMX708: attempt "); uart_hex(attempt);
        uart_puts(" -> ID=0x"); uart_hex(chip_id); uart_puts("\n");
        if (chip_id == 0x0708) {
            uart_puts("IMX708: Probe OK\n");
            return true;
        }
        for (volatile int i = 0; i < 100000; i++);
    }
    uart_puts("IMX708: Probe failed\n");
    return false;
}

uint8_t imx708_read_frame_count() {
    return imx708_read(0x0005);
}

void imx708_init_baseline() {
    /* V159: binned 1536×864 mode. Two-stage init mirrors the official
     * raspberrypi/linux imx708.c driver: common regs first, then mode regs. */
    uart_puts("IMX708: writing binned-mode regs (common=0x");
    uart_hex(k_imx708_common_len);
    uart_puts(", mode=0x");
    uart_hex(k_imx708_binned_len);
    uart_puts(")\n");

    for (int i = 0; i < k_imx708_common_len; i++)
        imx708_write(k_imx708_common[i].reg, k_imx708_common[i].val);

    for (int i = 0; i < k_imx708_binned_len; i++)
        imx708_write(k_imx708_binned[i].reg, k_imx708_binned[i].val);

    /* Stay in standby until stream_on */
    imx708_write(0x0100, 0x00);
    uart_puts("IMX708: binned-mode init complete (standby)\n");
}

void imx708_stream_on() {
    imx708_write(0x0100, 0x01);
}
