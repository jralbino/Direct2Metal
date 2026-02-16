/* File: src/data.s */
.section .rodata    /* Read-Only Data (Seccion de constantes) */
.global weights_start
.global weights_end
.align 4            /* Alinear a 4 bytes (crítico para float32) */

weights_start:
    .incbin "src/weights.bin"   /* El ensamblador chupa el archivo aqui */
weights_end: