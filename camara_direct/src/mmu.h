#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Build translation table + enable MMU on the calling core. Used by core 0. */
void init_mmu();

/* Enable MMU on the calling core, reusing the table already built by core 0.
 * MUST be called by each secondary core before it touches cacheable memory. */
void mmu_enable_this_core();

#ifdef __cplusplus
}
#endif
