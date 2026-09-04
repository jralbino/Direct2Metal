/*
 * raspbootin64 — serial kernel loader for D2M
 * Based on bztsrc/raspi3-tutorial 14_raspbootin64 (MIT, see NOTICE).
 *
 * Local change vs upstream: the upper size limit is raised from 1 MiB to
 * 16 MiB so it can also carry the full incbin build if ever needed. The D2M
 * serial workflow normally sends only the ~200 KiB code-only kernel and lets
 * the VPU load weights via `initramfs` (see tools/hwbench.py).
 */

#include "uart.h"

void main()
{
    int size = 0;
    char *kernel = (char*)0x80000;

    uart_init();

again:
    uart_send('R'); uart_send('B'); uart_send('I'); uart_send('N');
    uart_send('6'); uart_send('4'); uart_send('\r'); uart_send('\n');

    /* notify raspbootcom / hwbench.py to send the kernel */
    uart_send(3);
    uart_send(3);
    uart_send(3);

    /* read the kernel's size (little-endian) */
    size  =  uart_getc();
    size |=  uart_getc() << 8;
    size |=  uart_getc() << 16;
    size |=  uart_getc() << 24;

    if (size < 64 || size > 16 * 1024 * 1024) {
        uart_send('S');
        uart_send('E');
        goto again;
    }
    uart_send('O');
    uart_send('K');

    while (size--) *kernel++ = uart_getc();

    /* restore firmware arguments and jump to the freshly loaded kernel */
    asm volatile (
        "mov x0, x10;"
        "mov x1, x11;"
        "mov x2, x12;"
        "mov x3, x13;"
        "mov x30, 0x80000; ret"
    );
}
