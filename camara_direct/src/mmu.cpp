/* MMU — single-core port for camara_direct (BCM2837, Cortex-A53).
 *
 * Identity-maps 1 GB of PA using 2 MB blocks:
 *   [0x00000000 .. 0x3EFFFFFF) → Normal WB, Inner Shareable  (RAM)
 *   [0x3F000000 .. 0x40000000) → Device-nGnRnE, XN          (MMIO)
 *
 * Without this, D-cache stays off and CPU loops (debayer/deshift) run
 * at uncached memory speed — 10–50× slower than they should be. */

#include <stdint.h>
#include "mmu.h"

__attribute__((aligned(4096)))
static uint64_t translation_table[512];

#define MMIO_BASE     0x3F000000UL
#define VPU_BASE      0x1C000000UL   /* gpu_mem=128 → VPU owns last 128 MB below 0x20000000 */
#define VPU_END       0x20000000UL

#define MAIR_VALUE    ((0x00UL) | (0xFFUL << 8) | (0x44UL << 16))

#define PTE_VALID     (1UL << 0)
#define PTE_BLOCK     (0UL << 1)
#define PTE_ATTR(n)   ((n) << 2)
#define PTE_SH_INNER  (3UL << 8)
#define PTE_AF        (1UL << 10)
#define PTE_XN        (1UL << 54)

#define ATTR_DEVICE   0UL
#define ATTR_NORMAL   1UL
#define ATTR_NOCACHE  2UL

static inline void flush_table_to_ram(const void* addr, uint32_t size) {
    const uintptr_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end   = (((uintptr_t)addr + size) + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("dc civac, %0" :: "r"((void*)p));
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Build the global translation table — runs ONCE on core 0 only. Secondary
 * cores reuse the same table; rebuilding from a secondary would race. */
static void mmu_build_table() {
    uint64_t pa = 0;
    for (int i = 0; i < 512; i++, pa += 0x200000UL) {
        uint64_t entry = pa | PTE_VALID | PTE_BLOCK | PTE_AF;
        if (pa >= MMIO_BASE) {
            entry |= PTE_ATTR(ATTR_DEVICE) | PTE_XN;
        } else if (pa >= VPU_BASE && pa < VPU_END) {
            /* VPU heap incl. framebuffer — must be visible to VPU without
             * ARM cache sitting on top. Normal Non-cacheable allows write
             * combining (pixel stores stay fast) but every write lands in
             * SDRAM promptly, so the VPU scanout sees fresh pixels. */
            entry |= PTE_ATTR(ATTR_NOCACHE) | PTE_SH_INNER;
        } else {
            entry |= PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER;
        }
        translation_table[i] = entry;
    }

    flush_table_to_ram(translation_table, sizeof(translation_table));
}

/* Per-core MMU enable. All system registers written here are banked per-core
 * (MAIR_EL1, TCR_EL1, TTBR0_EL1, SCTLR_EL1) — each core sets its own. The
 * TTBR0 points at the shared table built by core 0 (`translation_table`). */
extern "C" void mmu_enable_this_core() {
    __asm__ volatile("tlbi vmalle1is");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");

    __asm__ volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE) : "memory");

    const uint64_t tcr = (34UL << 0) | (1UL << 8) | (1UL << 10) | (3UL << 12);
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");

    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)translation_table) : "memory");
    __asm__ volatile("isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ volatile("isb");
}

extern "C" void init_mmu() {
    mmu_build_table();
    mmu_enable_this_core();
}
