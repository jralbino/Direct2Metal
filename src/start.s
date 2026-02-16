/* File: src/start.s */
.section .text
.global _start

_start:
    /* 1. Check Core 0 only */
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    cbz x0, main_core
    b   hang

main_core:
    /* 2. Initialize Stack Pointer (Below 0x80000) */
    ldr x0, =0x80000
    mov sp, x0

    /* 3. Branch to C++ Main Function */
    bl kernel_main

    /* 4. If C++ returns, just hang */
hang:
    wfe
    b hang