/* HUD chrome — animated top + bottom bars. See hud.h for the layout. */
#include "hud.h"
#include <stdint.h>

extern void draw_pixel(int x, int y, uint32_t color);
extern void draw_rect(int x, int y, int w, int h, uint32_t color, int thickness);
extern void draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale);

/* ── COCO 80 (Ultralytics YOLOv5 order) ─────────────────────────────────────
 * Used by YOLOv5n.pt and weights.bin. */
const char* const coco_names[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck",
    "boat","traffic light","fire hydrant","stop sign","parking meter","bench",
    "bird","cat","dog","horse","sheep","cow","elephant","bear","zebra",
    "giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
    "skis","snowboard","sports ball","kite","baseball bat","baseball glove",
    "skateboard","surfboard","tennis racket","bottle","wine glass","cup",
    "fork","knife","spoon","bowl","banana","apple","sandwich","orange",
    "broccoli","carrot","hot dog","pizza","donut","cake","chair","couch",
    "potted plant","bed","dining table","toilet","tv","laptop","mouse",
    "remote","keyboard","cell phone","microwave","oven","toaster","sink",
    "refrigerator","book","clock","vase","scissors","teddy bear","hair drier",
    "toothbrush"
};

/* ── HUD state — written once per YOLO cycle, read every chrome render ───── */
#define HUD_MAX_PREDS 3            /* card slots in the bottom bar */
#define HUD_RECENT    5            /* short class-history rolling buffer */

struct {
    HudPred preds[HUD_MAX_PREDS];
    int     n_preds;
    int     frame_idx;
    int     inference_ms;
    int     ae_cit;
    /* Animated state */
    uint32_t tick;                 /* incremented each hud_render call */
    int      recent_classes[HUD_RECENT];
    int      recent_count;
} s_hud;

/* Colors (XRGB in BGR-low-24 convention to match draw_pixel) */
#define COL_BG       0xFF101820u
#define COL_BG_DIM   0xFF0A0F18u
#define COL_FG       0xFFE0E8F0u   /* light blue-white */
#define COL_FG_DIM   0xFF7080A0u
#define COL_ACCENT   0xFF00C0FFu   /* cyan accent */
#define COL_GOOD     0xFF00FF80u   /* green for confirmed detections */
#define COL_RED      0xFF4040FFu

void hud_init() {
    s_hud.n_preds      = 0;
    s_hud.frame_idx    = 0;
    s_hud.inference_ms = 0;
    s_hud.ae_cit       = 0;
    s_hud.tick         = 0;
    s_hud.recent_count = 0;
    for (int i = 0; i < HUD_MAX_PREDS; i++) {
        s_hud.preds[i].cls  = -1;
        s_hud.preds[i].conf = 0.0f;
    }
}

void hud_update_state(const HudPred* preds, int n_valid,
                      int frame_idx, int inference_ms, int ae_cit) {
    int n = (n_valid < HUD_MAX_PREDS) ? n_valid : HUD_MAX_PREDS;
    s_hud.n_preds      = n;
    s_hud.frame_idx    = frame_idx;
    s_hud.inference_ms = inference_ms;
    s_hud.ae_cit       = ae_cit;
    for (int i = 0; i < n; i++) s_hud.preds[i] = preds[i];
    for (int i = n; i < HUD_MAX_PREDS; i++) {
        s_hud.preds[i].cls  = -1;
        s_hud.preds[i].conf = 0.0f;
    }
    /* Roll recent_classes: insert the strongest new detection at the front */
    if (n > 0 && preds[0].cls >= 0 && preds[0].cls < 80) {
        for (int i = HUD_RECENT - 1; i > 0; i--) {
            s_hud.recent_classes[i] = s_hud.recent_classes[i-1];
        }
        s_hud.recent_classes[0] = preds[0].cls;
        if (s_hud.recent_count < HUD_RECENT) s_hud.recent_count++;
    }
}

/* Fill a rectangular region with a single color. Cheaper than draw_rect's
 * outline-only path; fb is XRGB8888, pitch in bytes. */
static void fill_rect(uint8_t* fb, uint32_t pitch, int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = (uint32_t*)(fb + (uint32_t)yy * pitch);
        for (int xx = x; xx < x + w; xx++) row[xx] = color;
    }
}

/* Animated FPS bar: a horizontal segmented gauge that fills proportionally
 * to ms (clamped 0..1000). Animates a "sweep" highlight at s_hud.tick. */
static void draw_fps_gauge(uint8_t* fb, uint32_t pitch, int x, int y, int w, int h,
                            int ms_per_inference) {
    /* Clamp inference time to 0..500 ms range for the gauge. */
    int clamped = ms_per_inference;
    if (clamped > 500) clamped = 500;
    int filled = (w * clamped) / 500;
    int seg_w  = 6;
    int seg_gap = 2;
    /* Background of bar */
    fill_rect(fb, pitch, x, y, w, h, COL_BG_DIM);
    /* Filled segments */
    for (int sx = 0; sx + seg_w <= filled; sx += seg_w + seg_gap) {
        /* Color goes green→yellow→red as inference gets slower */
        uint32_t segc = (sx < w / 3)       ? COL_GOOD
                      : (sx < (2 * w) / 3) ? 0xFF00FFFFu
                                           : COL_RED;
        fill_rect(fb, pitch, x + sx, y, seg_w, h, segc);
    }
    /* Animated sweep: a single bright pixel moving across the bar tied
     * to s_hud.tick. Period: ~60 ticks = ~1 second at 60 fps. */
    int sweep_x = (int)((s_hud.tick * w / 60u) % (uint32_t)w);
    fill_rect(fb, pitch, x + sweep_x, y, 2, h, COL_ACCENT);
}

/* Top bar (rows 0..59): title + FPS + frame count + AE CIT */
static void render_top_bar(uint8_t* fb, uint32_t pitch) {
    fill_rect(fb, pitch, 0, 0, 640, 60, COL_BG);
    /* Thin accent line at bottom of top bar */
    fill_rect(fb, pitch, 0, 58, 640, 2, COL_ACCENT);

    /* Title */
    draw_text(8, 6, "D2M YOLO", COL_FG, COL_BG, 2);

    /* FPS — display 1000/inference_ms with 1 decimal */
    char fps_buf[16]; int n = 0;
    int ms = s_hud.inference_ms;
    int fps_tenths = (ms > 0) ? (10000 / ms) : 0;
    fps_buf[n++] = 'F'; fps_buf[n++] = 'P'; fps_buf[n++] = 'S';
    fps_buf[n++] = ':'; fps_buf[n++] = ' ';
    int whole = fps_tenths / 10;
    if (whole >= 10) fps_buf[n++] = '0' + (whole / 10);
    fps_buf[n++] = '0' + (whole % 10);
    fps_buf[n++] = '.';
    fps_buf[n++] = '0' + (fps_tenths % 10);
    fps_buf[n] = '\0';
    draw_text(180, 6, fps_buf, COL_FG, COL_BG, 2);

    /* FPS animated gauge: 320 wide × 8 tall */
    draw_fps_gauge(fb, pitch, 180, 32, 200, 8, ms);

    /* Right side: frame count + AE CIT */
    char info_buf[24]; n = 0;
    info_buf[n++] = 'F'; info_buf[n++] = ' ';
    int fc = s_hud.frame_idx;
    if (fc >= 1000) { info_buf[n++] = '0' + (fc / 1000) % 10; }
    if (fc >= 100)  { info_buf[n++] = '0' + (fc / 100)  % 10; }
    if (fc >= 10)   { info_buf[n++] = '0' + (fc / 10)   % 10; }
    info_buf[n++] = '0' + fc % 10;
    info_buf[n] = '\0';
    draw_text(420, 6, info_buf, COL_FG_DIM, COL_BG, 2);

    /* AE CIT — short label */
    char ae_buf[24]; n = 0;
    ae_buf[n++] = 'C'; ae_buf[n++] = 'I'; ae_buf[n++] = 'T';
    ae_buf[n++] = ':'; ae_buf[n++] = ' ';
    int cit = s_hud.ae_cit;
    if (cit >= 1000) { ae_buf[n++] = '0' + (cit / 1000) % 10; }
    if (cit >= 100)  { ae_buf[n++] = '0' + (cit / 100)  % 10; }
    if (cit >= 10)   { ae_buf[n++] = '0' + (cit / 10)   % 10; }
    ae_buf[n++] = '0' + cit % 10;
    ae_buf[n] = '\0';
    draw_text(420, 32, ae_buf, COL_FG_DIM, COL_BG, 2);
}

/* Bottom bar (rows 420..479): up to 3 detection cards side by side. Each
 * card: class name + confidence% + small animated pulse if "fresh". */
static void render_bottom_bar(uint8_t* fb, uint32_t pitch) {
    fill_rect(fb, pitch, 0, 420, 640, 60, COL_BG);
    fill_rect(fb, pitch, 0, 420, 640, 2, COL_ACCENT);

    /* No detections → leave the bar empty (just the title strip). Avoids
     * any animation chrome that misbehaves at low refresh. */
    if (s_hud.n_preds == 0) return;

    /* Lay out up to 3 cards across 640 px: 200 px each + 10 gap. */
    const int card_w = 200;
    const int card_h = 50;
    const int card_y = 425;
    for (int i = 0; i < HUD_MAX_PREDS; i++) {
        int x = 15 + i * (card_w + 10);
        if (s_hud.preds[i].cls < 0) continue;

        /* Card background — slight gradient via two filled rects */
        fill_rect(fb, pitch, x, card_y, card_w, card_h, COL_BG_DIM);

        /* Left accent stripe — color by "freshness" (first card is brightest) */
        uint32_t stripe = (i == 0) ? COL_GOOD
                       : (i == 1) ? COL_ACCENT
                                  : COL_FG_DIM;
        fill_rect(fb, pitch, x, card_y, 4, card_h, stripe);

        /* Class name */
        int cls = s_hud.preds[i].cls;
        const char* nm = (cls >= 0 && cls < 80) ? coco_names[cls] : "?";
        draw_text(x + 10, card_y + 4, nm, COL_FG, COL_BG_DIM, 2);

        /* Confidence as integer % */
        int pct = (int)(s_hud.preds[i].conf * 100.0f);
        if (pct < 0)  pct = 0;
        if (pct > 99) pct = 99;
        char pbuf[8];
        int pn = 0;
        if (pct >= 10) pbuf[pn++] = '0' + (pct / 10);
        pbuf[pn++] = '0' + (pct % 10);
        pbuf[pn++] = '%';
        pbuf[pn] = '\0';
        draw_text(x + 10, card_y + 26, pbuf, COL_ACCENT, COL_BG_DIM, 2);

        /* Confidence bar — full width minus padding */
        int bar_x = x + 60;
        int bar_w = card_w - 70;
        int bar_h = 8;
        int bar_y = card_y + 32;
        int fill_w = (bar_w * pct) / 100;
        fill_rect(fb, pitch, bar_x, bar_y, bar_w, bar_h, COL_BG);
        fill_rect(fb, pitch, bar_x, bar_y, fill_w, bar_h, COL_ACCENT);
    }
}

void hud_render(uint8_t* fb, uint32_t pitch) {
    s_hud.tick++;
    render_top_bar(fb, pitch);
    render_bottom_bar(fb, pitch);
}
