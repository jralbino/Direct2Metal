/* src/kernel.cpp — BSP boot + main loop dispatcher.
 *
 * After Step 4 (2026-05-20) this file only holds the hardware bring-up
 * ceremony: UART, timer accessors, secondary-core spawn, MMU init,
 * camera/watchdog init, and the per-frame call to run_yolo_complete().
 * The model + inference live in yolo_v8n.cpp. */
#include <stdint.h>
#include "mmu.h"
#include "mailbox.h"
#include "watchdog.h"
#include "camera.h"
#include "hud.h"
#include "bsp.h"
#include "yolo_v8n.h"

volatile uint32_t* const UART0_DR = (uint32_t*)0x3F201000;
volatile uint32_t* const UART0_FR = (uint32_t*)0x3F201018;
volatile uint32_t* const UART0_CR = (uint32_t*)0x3F201030;

void uart_init() {
    *UART0_CR = 0;
    *((volatile uint32_t*)0x3F200004) = (*((volatile uint32_t*)0x3F200004) & ~((7 << 12) | (7 << 15))) | ((4 << 12) | (4 << 15));
    *((volatile uint32_t*)0x3F201044) = 0x7FF;
    *((volatile uint32_t*)0x3F201024) = 26;
    *((volatile uint32_t*)0x3F201028) = 3;
    *((volatile uint32_t*)0x3F20102C) = 0x70;
    *UART0_CR = 0x301;
}
void uart_putc(unsigned char c) { while (*UART0_FR & (1 << 5)); *UART0_DR = c; }
void uart_puts(const char* s) { while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); } }
extern "C" void uart_puts_c(const char* s) { uart_puts(s); }
void uart_dec(int n) {
    if (n < 0) { uart_putc('-'); n = -n; }
    if (n == 0) { uart_putc('0'); return; }
    char buf[20]; int i = 0;
    while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; }
    while (--i >= 0) uart_putc(buf[i]);
}

unsigned long get_timer_freq() { unsigned long v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
unsigned long get_timer_count() { unsigned long v; asm volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }

extern "C" void _start();

extern "C" void kernel_main() {
    uart_init(); uart_puts("\r\n=== Direct2Metal V170 (YOLOv8n 256×256 anchor-free DFL) ===\r\n");
    hud_init();

    mbox[0] = 7 * 4; mbox[1] = 0; mbox[2] = 0x00000001; mbox[3] = 4; mbox[4] = 0; mbox[5] = 0; mbox[6] = 0;
    if (!mbox_call(MBOX_CH_PROP)) uart_puts("[GPU] ERROR: No mailbox response\n");
    *(volatile uint64_t*)0xE0 = (uint64_t)&_start; *(volatile uint64_t*)0xE8 = (uint64_t)&_start; *(volatile uint64_t*)0xF0 = (uint64_t)&_start;
    asm volatile("sev");
    extern volatile int bss_ready; bss_ready = 1; flush_to_ram((void*)&bss_ready, 4);
    asm volatile("dsb sy" : : : "memory"); asm volatile("sev");
    video_init(); draw_fill(0xFF00FF00); video_flush();
    init_mmu();
    if (!camera_init()) uart_puts("[CAM] No camera — test mode\n");
    if (get_timer_freq() != 62500000UL) watchdog_init(4000);
#ifdef SIMULATION
    for (int _sim_frame = 0; _sim_frame < 3; _sim_frame++) run_yolo_complete();
    uart_puts("[SIM] All simulation frames complete — exiting QEMU.\n");
    register uint64_t _x0 asm("x0") = 0x20026;
    register uint64_t _x1 asm("x1") = 0;
    asm volatile("hlt #0xf000" :: "r"(_x0), "r"(_x1));
    __builtin_unreachable();
#else
    while(1) run_yolo_complete();
#endif
}
