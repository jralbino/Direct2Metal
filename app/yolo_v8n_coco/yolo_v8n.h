/* src/yolo_v8n.h — YOLOv8n COCO inference loop entry point.
 *
 * Step 4 (BSP/app split): the model graph, weights stream, NMS, decoder,
 * post-process and HUD update all live in yolo_v8n.cpp. kernel.cpp keeps
 * only the BSP boot logic and dispatches one call per frame to
 * run_yolo_complete().
 *
 * Step 5 will rename this to app/yolo_v8n_coco/main.h or split further. */
#pragma once

/* Run one frame end-to-end: acquire → preprocess → backbone → neck → head
 * → NMS → draw bboxes → HUD repaint → flush. Blocks until done. */
void run_yolo_complete();
