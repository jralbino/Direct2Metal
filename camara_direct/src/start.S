.section ".text.boot"

.global _start
.global secondary_entry

_start:
    mrs     x0, mpidr_el1
    and     x0, x0, #3
    cbz     x0, master              // core 0 → master path
                                    // cores 1-3 fall through to spin loop

// Spin on the armstub spin-table at 0xD8 + core_id*8 (Pi 3+ default armstub).
// On QEMU `-M raspi3b` cores 1-3 land here directly from _start. On real Pi
// hardware they're already spinning in armstub at 0x0; writing the entry to
// the spin slot from core 0 wakes them and they branch to `secondary_entry`.
secondary_spin:
    mrs     x9, mpidr_el1
    and     x9, x9, #3              // x9 = core_id (1, 2, or 3)
    mov     x10, #0xD8
    add     x10, x10, x9, lsl #3    // x10 = &spin_table[core_id]
1:  ldr     x0, [x10]
    cbz     x0, 2f
    br      x0                      // jump to entry written by core 0
2:  wfe
    b       1b

// Entry point that core 0 writes into the spin-table slots. Cores 1-3 land
// here (still at whatever EL armstub left them — usually EL2 on QEMU, EL1
// or EL2 on real Pi). We mirror core 0's drop-to-EL1 + FP/SIMD enable, then
// call C `secondary_main(core_id)`.
secondary_entry:
    // Per-core stack: top = secondary_stack_top, each core gets 16 KB downward.
    mrs     x9, mpidr_el1
    and     x9, x9, #3
    ldr     x1, =secondary_stack_top
    mov     x2, #16384
    mul     x2, x9, x2              // core 1=16K, core 2=32K, core 3=48K
    sub     x1, x1, x2
    mov     sp, x1

    // Drop to EL1 if at EL2 (mirrors the master path).
    mrs     x0, CurrentEL
    cmp     x0, #0x8
    b.ne    secondary_at_el1

    mrs     x0, cptr_el2
    bic     x0, x0, #(1 << 10)      // CPTR_EL2.TFP = 0
    msr     cptr_el2, x0
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, secondary_at_el1
    msr     elr_el2, x0
    isb
    eret

secondary_at_el1:
    // Re-establish SP after potential ERET.
    mrs     x9, mpidr_el1
    and     x9, x9, #3
    ldr     x1, =secondary_stack_top
    mov     x2, #16384
    mul     x2, x9, x2
    sub     x1, x1, x2
    mov     sp, x1

    // FP/SIMD on this core (CPACR_EL1.FPEN = 0b11).
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    // Pass core_id to C and call secondary_main.
    mrs     x0, mpidr_el1
    and     x0, x0, #3
    bl      secondary_main

secondary_park:
    wfe
    b       secondary_park

master:
    // Clear BSS first — no stack needed for this loop.
    ldr     x0, =__bss_start
    ldr     w1, =__bss_size
cbss:
    cbz     w1, drop_to_el1
    str     xzr, [x0], #8
    sub     w1, w1, #1
    cbnz    w1, cbss

drop_to_el1:
    // QEMU raspi3b (and Pi firmware) drop us here at EL2.
    // MMU EL1 registers only affect our execution once we're at EL1h.
    mrs     x0, CurrentEL
    cmp     x0, #0x8
    b.ne    setup_env

    // Clear CPTR_EL2.TFP (bit 10) so FP/SIMD at EL1/EL0 doesn't trap to EL2.
    mrs     x0, cptr_el2
    bic     x0, x0, #(1 << 10)
    msr     cptr_el2, x0

    mov     x0, #(1 << 31)      // HCR_EL2.RW = 1 → EL1 is AArch64
    msr     hcr_el2, x0
    mov     x0, #0x3c5          // D,A,I,F masked, EL1h
    msr     spsr_el2, x0
    adr     x0, setup_env
    msr     elr_el2, x0
    isb
    eret

setup_env:
    // Now at EL1. SP_EL1 is uninitialised — set it.
    ldr     x0, =_start
    mov     sp, x0

    // Enable FP/SIMD at EL0/EL1 (CPACR_EL1.FPEN bits[21:20] = 0b11).
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

run_main:
    bl      init_mmu
    bl      kernel_main

master_park:
    wfe
    b       master_park

// 16 KB stack per secondary core (cores 1, 2, 3) → 48 KB total.
// Each core gets [secondary_stack_top - core_id*16K, secondary_stack_top - (core_id-1)*16K).
.section ".bss"
.align 4
.global secondary_stack_base
secondary_stack_base:
    .skip 16384 * 3
.global secondary_stack_top
secondary_stack_top:
