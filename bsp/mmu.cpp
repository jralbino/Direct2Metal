/* File: src/mmu.cpp - FIXED COHERENCY */
#include <stdint.h>
#include "mmu.h"

/* Importamos la función de limpieza desde multicore.cpp */
extern "C" void flush_to_ram(volatile void* addr, unsigned long size);

__attribute__((aligned(4096)))
static uint64_t translation_table[512];

/* Variable de sincronización */
volatile int mmu_table_ready = 0;

#define MMIO_BASE 0x3F000000UL
#define MAIR_VALUE  ((0x00UL) | (0xFFUL << 8) | (0x44UL << 16))

/* Page Table Entry Attributes */
#define PTE_VALID      (1UL << 0)
#define PTE_BLOCK      (0UL << 1)
#define PTE_ATTR(n)    ((n) << 2)
#define PTE_SH_INNER   (3UL << 8)
#define PTE_AF         (1UL << 10)
#define PTE_XN         (1UL << 54)

#define ATTR_DEVICE 0UL
#define ATTR_NORMAL 1UL
#define ATTR_NOCACHE 2UL

void init_mmu() {
    int core_id = 0;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    if (core_id == 0) {
        // --- CORE 0: CREA LA TABLA ---
        uint64_t pa = 0;
        for (int i = 0; i < 512; i++, pa += 0x200000UL) {
            uint64_t entry = pa | PTE_VALID | PTE_BLOCK | PTE_AF;
            if (pa >= MMIO_BASE) {
                entry |= PTE_ATTR(ATTR_DEVICE) | PTE_XN;
            } else if (pa >= 0x1E000000UL && pa < 0x20000000UL) {
                entry |= PTE_ATTR(ATTR_NOCACHE) | PTE_SH_INNER;
            } else {
                entry |= PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER;
            }
            translation_table[i] = entry;
        }
        
        // CRÍTICO: Asegurar que la tabla esté escrita en RAM
        flush_to_ram(translation_table, sizeof(translation_table));
        asm volatile("dsb sy" ::: "memory");

        // Señalizar que estamos listos
        __atomic_store_n(&mmu_table_ready, 1, __ATOMIC_RELEASE);
        
        // CRÍTICO: Empujar la bandera a RAM para que los cores sin MMU la vean
        flush_to_ram(&mmu_table_ready, sizeof(int));
        asm volatile("dsb sy" ::: "memory");
        asm volatile("sev"); // Despertar a cualquiera durmiendo

    } else {
        // --- CORES 1-3: ESPERAN ---
        // Al leer sin MMU, leen RAM directa. Gracias al flush arriba, leerán '1'.
        while (__atomic_load_n(&mmu_table_ready, __ATOMIC_ACQUIRE) == 0) {
            // Pequeña pausa para no saturar el bus
            asm volatile("wfe"); 
        }
    }

    // --- TODOS: ACTIVAN MMU ---
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    asm volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE) : "memory");
    
    // T0SZ=34 (1GB space), Cacheable page tables
    const uint64_t tcr = (34UL << 0) | (1UL << 8) | (1UL << 10) | (3UL << 12);
    asm volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    
    asm volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)translation_table) : "memory");
    asm volatile("isb");

    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12); // M + C + I
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    asm volatile("isb");
}