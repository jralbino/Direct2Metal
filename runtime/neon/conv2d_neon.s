/* File: src/conv2d_neon.s (Fixed LR Version) */
.section .text
.global neon_kernel_3x3

/* * void neon_kernel_3x3(...)
 * x0: Input ptr
 * x1: Weights ptr
 * x2: Output ptr
 * x3: Stride bytes
 */
neon_kernel_3x3:
    /* CRITICO: Guardar x29 (Frame Pointer) y x30 (Link Register) */
    /* Sin esto, 'bl' destruye la dirección de retorno a C++ */
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    
    /* Guardar registros callee-saved si los usáramos (x19-x28). 
       En este caso usamos x9-x15 (scratch), así que no hace falta salvarlos. */

    /* Inicializar acumulador v0 en cero */
    movi v0.4s, #0

    /* --- CANAL 0 --- */
    bl process_3x3_channel
    
    /* Avanzar input al Canal 1 (16x16 floats = 256 floats = 1024 bytes) */
    add x0, x0, #1024 
    bl process_3x3_channel

    /* Avanzar input al Canal 2 */
    add x0, x0, #1024
    bl process_3x3_channel

    /* Guardar resultado */
    st1 {v0.4s}, [x2]

    /* Restaurar pila y volver a C++ */
    ldp x29, x30, [sp], #16
    ret

/* --- SUBRUTINA --- */
process_3x3_channel:
    mov x9, x0  /* Copia temporal del puntero base */

    /* FILA 0 */
    ldr s4, [x9]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s
    
    ldr s4, [x9, #4]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    ldr s4, [x9, #8]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    /* FILA 1 */
    add x9, x9, x3 /* Stride */
    
    ldr s4, [x9]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    ldr s4, [x9, #4]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    ldr s4, [x9, #8]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    /* FILA 2 */
    add x9, x9, x3 /* Stride */

    ldr s4, [x9]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    ldr s4, [x9, #4]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    ldr s4, [x9, #8]
    dup v4.4s, v4.s[0]
    ld1 {v5.4s}, [x1], #16
    fmla v0.4s, v4.4s, v5.4s

    ret