/* File: src/matmul_neon.s (v2 - Register Accumulation) */
.section .text
.global matmul_neon

matmul_neon:
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!

    mov x4, #0              /* i = 0 */

loop_i:
    cmp x4, x3
    b.ge end_func

    mov x6, #0              /* j = 0 */

loop_j:
    cmp x6, x3
    b.ge next_i

    /* 1. Inicializar Acumulador (v2) en CERO */
    /* Vamos a sumar todo aquí y solo guardar al final */
    movi v2.4s, #0

    mov x5, #0              /* k = 0 */

loop_k:
    cmp x5, x3
    b.ge store_result       /* Cuando K termine, guardamos */

    /* 2. Cargar A[i][k] -> Duplicar en v0 */
    mov x10, x4
    lsl x10, x10, #4
    add x10, x10, x5
    lsl x10, x10, #2
    ldr w11, [x0, x10]
    dup v0.4s, w11

    /* 3. Cargar B[k][j...j+3] -> v1 */
    mov x12, x5
    lsl x12, x12, #4
    add x12, x12, x6
    lsl x12, x12, #2
    add x9, x1, x12
    ld1 {v1.4s}, [x9]

    /* 4. Acumular en Registro (NO TOCAMOS RAM DE C AQUÍ) */
    mla v2.4s, v1.4s, v0.4s

    add x5, x5, #1          /* k++ */
    b loop_k

store_result:
    /* 5. AHORA sí guardamos el resultado final en C[i][j...j+3] */
    mov x13, x4
    lsl x13, x13, #4
    add x13, x13, x6
    lsl x13, x13, #2
    add x10, x2, x13
    
    st1 {v2.4s}, [x10]      /* Escribir en RAM 1 vez por cada 16 operaciones */

    add x6, x6, #4          /* j += 4 */
    b loop_j

next_i:
    add x4, x4, #1          /* i++ */
    b loop_i

end_func:
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret