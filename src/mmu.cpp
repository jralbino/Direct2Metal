/* File: src/mmu.cpp - Identity Mapping for RPi Zero 2 W (AArch64)
 *
 * Sets up a 1:1 (VA = PA) mapping over the first 1 GB and enables
 * the MMU, D-Cache, and I-Cache so inference runs with full hardware
 * acceleration.
 *
 * Design:
 *   T0SZ = 34  →  VA space = 2^(64-34) = 2^30 = 1 GB
 *              →  Starting level = 2 (4KB granule)
 *   Table:     512 Level-2 Block descriptors × 2 MB = 1 GB
 *
 * Memory type split:
 *   0x00000000 – 0x3EFFFFFF  Normal Write-Back Cached  (RAM)
 *   0x3F000000 – 0x3FFFFFFF  Device-nGnRnE Non-Cached  (MMIO)
 */

#include <stdint.h>
#include "mmu.h"

/* ----------------------------------------------------------------
 * Translation table: 512 entries × 8 bytes = 4096 bytes, 4KB-aligned.
 * Placed in BSS (zeroed before kernel_main by start.s).
 * ---------------------------------------------------------------- */
__attribute__((aligned(4096)))
static uint64_t translation_table[512];

/* BCM2837 MMIO window */
#define MMIO_BASE 0x3F000000UL

/* ----------------------------------------------------------------
 * MAIR_EL1 encoding:
 *   Attr[0] = 0x00  Device-nGnRnE   (no gather/reorder/early-write-ack)
 *   Attr[1] = 0xFF  Normal WB Inner+Outer Read/Write-Allocate
 * ---------------------------------------------------------------- */
#define MAIR_VALUE  ((0x00UL) | (0xFFUL << 8))
#define ATTR_DEVICE 0UL   /* index into MAIR */
#define ATTR_NORMAL 1UL

/* ----------------------------------------------------------------
 * Level-2 Block Descriptor bits (AArch64, 4KB granule)
 * ---------------------------------------------------------------- */
#define PTE_VALID      (1UL << 0)   /* entry is valid */
#define PTE_BLOCK      (0UL << 1)   /* Level-2 block (NOT a table) */
#define PTE_ATTR(n)    ((n) << 2)   /* AttrIndx[2:0] at bits [4:2] */
#define PTE_SH_INNER   (3UL << 8)   /* Inner Shareable */
#define PTE_AF         (1UL << 10)  /* Access Flag — must be set or AF fault */
#define PTE_XN         (1UL << 54)  /* Execute Never */

void init_mmu() {
    /* ============================================================
     * Step 1 — Fill the Level-2 translation table (identity map)
     * ============================================================ */
    uint64_t pa = 0;
    for (int i = 0; i < 512; i++, pa += 0x200000UL) {
        uint64_t entry = pa | PTE_VALID | PTE_BLOCK | PTE_AF;

        if (pa >= MMIO_BASE) {
            /* Peripheral memory: Device-nGnRnE, Execute Never.
             * Accesses go directly to hardware, no caching. */
            entry |= PTE_ATTR(ATTR_DEVICE) | PTE_XN;
        } else {
            /* RAM: Normal WB cached, Inner Shareable.
             * D-cache and I-cache will be used for this region. */
            entry |= PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER;
        }

        translation_table[i] = entry;
    }

    /* ============================================================
     * Step 2 — Ensure table writes reach RAM before the MMU reads them
     * ============================================================ */
    asm volatile("dsb sy" ::: "memory");

    /* ============================================================
     * Step 3 — Invalidate all EL1 TLB entries
     *          (boot state is unpredictable per ARM ARM)
     * ============================================================ */
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    /* ============================================================
     * Step 4 — Write system registers
     *
     * Order: MAIR → TCR → TTBR0 → ISB (context sync) → SCTLR → ISB
     * ============================================================ */

    /* MAIR_EL1: define memory attribute indices */
    asm volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE) : "memory");

    /* TCR_EL1: translation control
     *   T0SZ   [5:0]  = 34  → 1 GB VA space, Level-2 starting table
     *   IRGN0  [9:8]  = 01  → Inner WB Write-Alloc (cacheable walk)
     *   ORGN0  [11:10]= 01  → Outer WB Write-Alloc
     *   SH0    [13:12]= 11  → Inner Shareable
     *   TG0    [15:14]= 00  → 4 KB granule (default)
     *
     * BUG NOTE: T0SZ must be 34, NOT 25.
     *   T0SZ=25 → Level-1 table (1 GB entries) with 2 MB step values
     *           → MMIO gets mapped as Normal WB cached instead of Device
     *           → UART writes go to cache, never reach hardware → hang
     *   T0SZ=34 → Level-2 table (2 MB entries), 512 entries × 2 MB = 1 GB ✓
     */
    const uint64_t tcr = (34UL <<  0)   /* T0SZ  */
                       | ( 1UL <<  8)   /* IRGN0 */
                       | ( 1UL << 10)   /* ORGN0 */
                       | ( 3UL << 12);  /* SH0   */
    asm volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");

    /* TTBR0_EL1: point to our page table */
    asm volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)translation_table) : "memory");

    /* ISB: context synchronisation — all register writes above are now
     * visible to subsequent MMU hardware walks. */
    asm volatile("isb");

    /* ============================================================
     * Step 5 — Enable MMU + D-Cache + I-Cache in SCTLR_EL1
     *   Bit 0  (M)  Enable MMU
     *   Bit 2  (C)  Enable D-Cache (Normal WB regions become cached)
     *   Bit 12 (I)  Enable I-Cache
     * ============================================================ */
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");

    /* ISB: activate the new translation regime.
     * Instruction fetches after this point go through the MMU. */
    asm volatile("isb");
}
