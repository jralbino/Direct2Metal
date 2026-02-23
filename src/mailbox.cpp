#include <stdint.h>

extern "C" void flush_to_ram(volatile void* addr, unsigned long size);
extern void uart_puts(const char* s);
extern void uart_hex(uint32_t d);

volatile uint32_t* const MBOX_READ   = (uint32_t*)0x3F00B880;
volatile uint32_t* const MBOX_STATUS = (uint32_t*)0x3F00B898;
volatile uint32_t* const MBOX_WRITE  = (uint32_t*)0x3F00B8A0;

#define MBOX_FULL  0x80000000
#define MBOX_EMPTY 0x40000000

__attribute__((aligned(16))) volatile uint32_t mbox[36];

int mbox_call(unsigned char ch) {
    flush_to_ram((volatile void*)&mbox, sizeof(mbox));

    // Convertir a 32 bits y aplicar el canal
    uint32_t r = (((uint32_t)((unsigned long)&mbox) & ~0xF) | (ch & 0xF));
    
    // Alias de bus (Uncached) para VideoCore
    r |= 0xC0000000; 

    while (*MBOX_STATUS & MBOX_FULL);
    *MBOX_WRITE = r;

    while (1) {
        while (*MBOX_STATUS & MBOX_EMPTY);
        uint32_t res = *MBOX_READ;
        if ((res & ~0xF) == (r & ~0xF)) {
            flush_to_ram((volatile void*)&mbox, sizeof(mbox));
            return mbox[1] == 0x80000000; // 0x80000000 = MBOX_SUCCESS
        }
    }
    return 0;
}