.global _start

/* ----------------------------------------------------------------
 * bss_ready — flag in .data (not .bss) so it has a defined value
 * (0) even before BSS is cleared.  Core 0 sets it to 1 in
 * kernel_main() after BSS is clear; secondary cores poll it before
 * entering secondary_main().
 * ---------------------------------------------------------------- */
.section .data
.align 2
.global bss_ready
bss_ready: .word 0

/* ----------------------------------------------------------------
 * Boot entry
 * ---------------------------------------------------------------- */
.section .text.boot

_start:
    /* 1. Read core ID from MPIDR_EL1 [1:0].
     *    Keep it in x19 (callee-saved in AArch64) — survives eret. */
    mrs  x19, mpidr_el1
    and  x19, x19, #3

    /* 2. EL2 → EL1 transition (required on all 4 cores). */
check_el:
    mrs  x0, CurrentEL
    lsr  x0, x0, #2
    cmp  x0, #2
    beq  switch_to_el1        /* at EL2: do the transition            */

    /* Already EL1 (unusual, but handle gracefully). */
    cbnz x19, secondary_el1
    b    el1_entry

switch_to_el1:
    /* HCR_EL2.RW = 1 → EL1 runs as AArch64. */
    ldr  x0, =0x80000000
    msr  hcr_el2, x0

    /* SPSR_EL2 = 0x3c5  →  EL1h, DAIF all masked. */
    ldr  x0, =0x3c5
    msr  spsr_el2, x0

    /* ELR_EL2: choose destination based on core ID. */
    cbnz x19, eret_secondary
    ldr  x0, =el1_entry
    msr  elr_el2, x0
    eret

eret_secondary:
    ldr  x0, =secondary_el1
    msr  elr_el2, x0
    eret

/* ----------------------------------------------------------------
 * secondary_el1 — entry for cores 1-3 after EL2→EL1 transition.
 * ---------------------------------------------------------------- */
secondary_el1:
    /* Enable FPU / NEON (CPACR_EL1.FPEN = 0b11). */
    mov  x0, #(3 << 20)
    msr  cpacr_el1, x0
    isb

    /* Per-core stack: sp = 0x80000 - core_id * 0x10000.
     *   Core 1: 0x70000 (64 KB below image base)
     *   Core 2: 0x60000
     *   Core 3: 0x50000                                         */
    ldr  x0, =0x80000
    mov  x1, #0x10000
    mul  x1, x19, x1
    sub  x0, x0, x1
    mov  sp, x0

    /* Wait for core 0 to clear BSS.  bss_ready lives in .data and
     * is 0 in the ELF binary, so this is race-free.             */
    ldr  x0, =bss_ready
wait_bss:
    ldar w1, [x0]           /* load-acquire */
    cbz  w1, wait_bss

    /* Enter C++ worker — never returns. */
    bl   secondary_main
secondary_hang:
    wfe
    b    secondary_hang

/* ----------------------------------------------------------------
 * el1_entry — Core 0 only.
 * ---------------------------------------------------------------- */
el1_entry:
    /* Enable FPU / NEON. */
    mov  x0, #(3 << 20)
    msr  cpacr_el1, x0
    isb

    /* Stack pointer at image base (grows downward). */
    ldr  x0, =_start
    mov  sp, x0

    /* Clear BSS. */
    ldr  x0, =__bss_start
    ldr  x1, =__bss_end
    sub  x1, x1, x0
    cbz  x1, run_kernel

clear_bss:
    str  xzr, [x0], #8
    sub  x1, x1, #8
    cbnz x1, clear_bss

run_kernel:
    /* Jump to C++. */
    bl   kernel_main

hang:
    wfe
    b    hang
