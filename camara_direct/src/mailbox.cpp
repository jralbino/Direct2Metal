#include "mailbox.h"
#include "uart.h"
#include <stdint.h>

#define MAILBOX_BASE    0x3F00B880
#define MAILBOX_READ    (*(volatile uint32_t*)(MAILBOX_BASE + 0x00))
#define MAILBOX_STATUS  (*(volatile uint32_t*)(MAILBOX_BASE + 0x18))
#define MAILBOX_WRITE   (*(volatile uint32_t*)(MAILBOX_BASE + 0x20))

#define MAILBOX_FULL    0x80000000
#define MAILBOX_EMPTY   0x40000000

volatile uint32_t __attribute__((aligned(64))) mailbox_buffer[32];

/* Clean-to-PoC so the VPU (which reads SDRAM directly via the 0xC0000000
 * alias) sees our writes; then dsb sy to serialise with the MMIO door-bell. */
static inline void dcache_clean_range(const volatile void* addr, uint32_t len) {
    const uintptr_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end   = (((uintptr_t)addr + len) + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("dc cvac, %0" :: "r"((void*)p));
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void dcache_inv_range(volatile void* addr, uint32_t len) {
    const uintptr_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end   = (((uintptr_t)addr + len) + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("dc ivac, %0" :: "r"((void*)p));
    __asm__ volatile("dsb sy" ::: "memory");
}

bool mailbox_call(uint8_t channel) {
    // Aplicar alias 0xC0000000 para que la GPU acceda directamente a RAM (L2 bypass)
    uint32_t r = (((uint32_t)((uintptr_t)mailbox_buffer) | 0xC0000000) & ~0xF) | (channel & 0xF);

    /* Flush the request out of ARM L1/L2 so the VPU sees it in SDRAM. */
    dcache_clean_range(mailbox_buffer, sizeof(mailbox_buffer));

    while (MAILBOX_STATUS & MAILBOX_FULL);
    MAILBOX_WRITE = r;

    while (1) {
        while (MAILBOX_STATUS & MAILBOX_EMPTY);
        uint32_t res = MAILBOX_READ;
        if ((res & 0xF) == channel) {
            if ((res & ~0xF) == (r & ~0xF)) {
                /* VPU wrote the response via the 0xC0 alias → our cached
                 * view may be stale. Invalidate before reading. */
                dcache_inv_range(mailbox_buffer, sizeof(mailbox_buffer));
                bool success = (mailbox_buffer[1] == 0x80000000);
                if (!success) {
                    uart_puts("MBOX: Response ERROR 0x"); uart_hex(mailbox_buffer[1]); uart_puts("\n");
                }
                return success;
            }
        }
    }
}

bool mailbox_set_domain_state(uint32_t domain, uint32_t state) {
    mailbox_buffer[0] = 8 * 4;
    mailbox_buffer[1] = 0;
    mailbox_buffer[2] = 0x00038030;
    mailbox_buffer[3] = 8;
    mailbox_buffer[4] = 0;
    mailbox_buffer[5] = domain;
    mailbox_buffer[6] = state;
    mailbox_buffer[7] = 0;
    return mailbox_call(8);
}

bool mailbox_set_clock_rate(uint32_t clock_id, uint32_t rate_hz) {
    // 9 enteros de 32 bits en total * 4 bytes cada uno = 36 bytes
    mailbox_buffer[0] = 9 * 4;      // Tamaño total del buffer
    mailbox_buffer[1] = 0;          // Código: Request

    // Tag: Set Clock Rate
    mailbox_buffer[2] = 0x00038002; // Tag ID
    mailbox_buffer[3] = 12;         // Tamaño del buffer de valor (3 enteros)
    mailbox_buffer[4] = 12;         // Tamaño de la petición
    mailbox_buffer[5] = clock_id;   // ID del reloj (4)
    mailbox_buffer[6] = rate_hz;    // Frecuencia (250000000)
    mailbox_buffer[7] = 0;          // Turbo (0 = no forzar turbo)

    // Tag de cierre
    mailbox_buffer[8] = 0;

    return mailbox_call(8);
}