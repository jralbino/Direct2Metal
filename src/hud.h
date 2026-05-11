/* HUD overlay — animated chrome between YOLO inference cycles.
 *
 * Layout on the 640×480 framebuffer:
 *   y= 0.. 59  top bar     — title + animated FPS gauge + AE CIT
 *   y=60..419  camera area — debayered image + bboxes (unchanged from V165)
 *   y=420..479 bottom bar  — last-detection cards with class names
 *
 * The two bars previously rendered as solid black letterbox. The HUD reuses
 * that real estate without enlarging the FB or losing the camera display.
 *
 * Driving rate: HUD chrome animates at ~60 fps during the gap between
 * consecutive YOLO inferences. The camera-area pixels are NOT touched by the
 * HUD redraws — only the two bars are repainted. */
#pragma once
#include <stdint.h>

/* COCO-80 class names (Ultralytics order). 80 entries, NULL-terminator at 80. */
extern const char* const coco_names[80];

/* Snapshot a single YOLO detection cycle's predictions into the HUD state.
 * Called at most once per YOLO cycle. preds is the same Box layout from
 * kernel.cpp run_yolo_complete (x, y, w, h, conf, cls). After NMS dedup.
 * n_valid is the count of preds with conf > CONF_THRESH (already sorted by
 * confidence descending). frame_idx is the global frame counter for the
 * "frame" KPI. inference_ms is the wall-clock time of the YOLO call. */
struct HudPred { int cls; float conf; };
void hud_update_state(const HudPred* preds, int n_valid,
                      int frame_idx, int inference_ms, int ae_cit);

/* Render the HUD chrome (top + bottom bars) into the framebuffer. Does NOT
 * touch the camera-area rows 60..419. Call this in a tight loop between
 * YOLO inferences — the perceived chrome rate is much higher than YOLO's
 * inference rate. */
void hud_render(uint8_t* fb, uint32_t pitch);

/* Initialize HUD state (zero detections, frame=0). Safe to call multiple
 * times. */
void hud_init();
