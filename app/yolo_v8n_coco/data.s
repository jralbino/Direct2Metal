/* app/yolo_v8n_coco/data.s — YOLO weight blob + test image, .incbin'd
 * into the kernel image. Step 5 moved these from src/ to app/yolo_v8n_coco/.
 * Paths below are relative to make's CWD (the project root). */
.section .rodata    /* Read-Only Data */
.align 4            /* 4-byte alignment (critical for float32) */

.global weights_start
.global weights_end
weights_start:
    .incbin "app/yolo_v8n_coco/weights.bin"
weights_end:

.align 4
.global test_image
.global test_image_end
test_image:
    .incbin "app/yolo_v8n_coco/test_image.bin"
test_image_end: