/* File: src/start.s - FULL RESTORATION (EL2->EL1, NEON, BSS SYNC) */
.section ".text.boot"
.global _start

_start:
    // 1. Identificar Core ID
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, core_zero

core_slave:
    // 2. Esclavos esperan a que Core 0 limpie BSS y de la senal
    ldr     x2, =bss_ready
1:  wfe
    ldr     w3, [x2]
    cbz     w3, 1b
    b       setup_env

core_zero:
    // 3. Core 0 limpia toda la seccion BSS con ceros
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
2:  cmp     x1, x2
    b.eq    setup_env
    str     xzr, [x1], #8
    b       2b

setup_env:
    // 4. Bajar de Hypervisor (EL2) a Kernel (EL1) si es necesario
    mrs     x0, CurrentEL
    cmp     x0, #0x8
    b.ne    enable_neon

    // Configurar registros de EL2 para saltar a EL1
    mov     x0, #(1 << 31)      // Modo AArch64
    msr     hcr_el2, x0
    mov     x0, #0x3c5          // Enmascarar D, A, I, F; modo EL1h
    msr     spsr_el2, x0
    adr     x0, enable_neon
    msr     elr_el2, x0
    isb                         // S9: ARM DDI0487C.d — sync SPSR/ELR writes before eret
    eret

enable_neon:
    // 5. ENCENDER COPROCESADOR NEON SIMD (El error fatal corregido)
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)  // Bits 20 y 21 a '1'
    msr     cpacr_el1, x0
    isb

    // 6. Configurar Puntero de Pila (Stack Pointer) separado por nucleo
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    ldr     x2, =0x80000
    lsl     x3, x1, #16         // Desplazar 64KB (0x10000) por nucleo
    sub     x2, x2, x3
    mov     sp, x2

    // S6: escribir canario al fondo de la pila de este nucleo
    // Fondo = sp_base - 64KB; asegura deteccion de stack overflow
    ldr     x4, =0xDEADBEEFDEADBEEF
    mov     x5, #0x10000
    sub     x5, x2, x5          // x5 = bottom of stack (sp_base - 64KB)
    str     x4, [x5]            // write canary at absolute stack bottom

    // 7. Salto al codigo C++
    cbz     x1, run_master
    bl      secondary_main
    b       hang

run_master:
    bl      kernel_main

hang:
    wfe
    b       hang

// Semáforo en .data que sobrevive la limpieza del BSS
.section ".data"
.align 2
.global bss_ready
bss_ready: .word 0
