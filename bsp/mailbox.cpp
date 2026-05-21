/* File: src/mailbox.cpp - VIDEOCORE MAILBOX INTERFACE */
#include "mailbox.h"

// El buffer del Mailbox DEBE estar alineado a 16 bytes.
__attribute__((aligned(16))) volatile uint32_t mbox[36];

// Helpers para limpiar caché
static inline void data_sync_barrier() { asm volatile("dsb sy" : : : "memory"); }
static inline void data_mem_barrier()  { asm volatile("dmb sy" : : : "memory"); }

int mbox_call(unsigned char ch) { // <--- CAMBIO AQUÍ
    uint32_t r = (((uint32_t)((uint64_t)&mbox) & ~0xF) | (ch & 0xF));

    while (*MAILBOX_STATUS & MAILBOX_FULL) { asm volatile("nop"); }

    data_sync_barrier();
    *MAILBOX_WRITE = r;
    data_mem_barrier();

    while (1) {
        while (*MAILBOX_STATUS & MAILBOX_EMPTY) { asm volatile("nop"); }
        
        data_sync_barrier();
        uint32_t data = *MAILBOX_READ;
        data_mem_barrier();

        if ((data & 0xF) == ch) {
            return mbox[1] == 0x80000000;
        }
    }
    return 0;
}