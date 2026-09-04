/* app/yolo_v8n_coco/data.s — YOLO weight blobs + test image, .incbin'd
 * into the kernel image. Paths below are relative to make's CWD (project
 * root). G2 (2026-05-20) added the INT8 blob alongside the FP32 one;
 * both are linked, the yolo_v8n.cpp build flag picks which to consume. */
.section .rodata    /* Read-Only Data */
.align 4            /* 4-byte alignment (critical for float32) */

.global weights_start
.global weights_end
weights_start:
    .incbin "app/yolo_v8n_coco/weights.bin"
weights_end:

.align 4
.global weights_int8_start
.global weights_int8_end
weights_int8_start:
    .incbin "app/yolo_v8n_coco/weights_int8.bin"
weights_int8_end:

.align 4
.global weights_int8_w8a8_start
.global weights_int8_w8a8_end
weights_int8_w8a8_start:
    .incbin "app/yolo_v8n_coco/weights_int8_w8a8.bin"
weights_int8_w8a8_end:

.align 4
.global test_image
.global test_image_end
test_image:
    .incbin "app/yolo_v8n_coco/test_image.bin"
test_image_end: