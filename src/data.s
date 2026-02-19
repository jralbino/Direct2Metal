/* File: src/data.s */
.section .rodata    /* Read-Only Data */
.align 4            /* 4-byte alignment (critical for float32) */

.global weights_start
.global weights_end
weights_start:
    .incbin "src/weights.bin"
weights_end:

.align 4
.global test_image
.global test_image_end
test_image:
    .incbin "src/test_image.bin"
test_image_end: