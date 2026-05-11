#include <stdint.h>

extern volatile uint32_t mbox[36];
extern int mbox_call(unsigned char ch);
extern "C" void flush_to_ram(volatile void* addr, unsigned long size);
extern void uart_puts(const char* s); 
extern void uart_dec(int n);

void uart_hex(uint32_t d) {
    uart_puts("0x");
    for(int i=28; i>=0; i-=4) {
        int n = (d >> i) & 0xF;
        n += n > 9 ? 0x37 : 0x30;
        /* Wait for TX FIFO not full BEFORE writing (fixes character drop bug) */
        while (*((volatile uint32_t*)0x3F201018) & (1 << 5));
        *((volatile uint32_t*)0x3F201000) = n;
    }
}

uint32_t width, height, pitch, isrgb;
unsigned char* lfb = 0; 

void video_init() {
    uart_puts("\n[VIDEO] Init 640x480...\n");

    mbox[0] = 35 * 4; 
    mbox[1] = 0;      

    // 1. Set Physical Display (640x480)
    mbox[2] = 0x48003; mbox[3] = 8; mbox[4] = 8; mbox[5] = 640; mbox[6] = 480;

    // 2. Set Virtual Buffer (640x480)
    mbox[7] = 0x48004; mbox[8] = 8; mbox[9] = 8; mbox[10] = 640; mbox[11] = 480;

    // 3. Set Virtual Offset 
    mbox[12] = 0x48009; mbox[13] = 8; mbox[14] = 8; mbox[15] = 0; mbox[16] = 0;

    // 4. Set Depth (32 bits)
    mbox[17] = 0x48005; mbox[18] = 4; mbox[19] = 4; mbox[20] = 32;

    // 5. Set Pixel Order (1 = RGB)
    mbox[21] = 0x48006; mbox[22] = 4; mbox[23] = 4; mbox[24] = 1;

    // 6. Allocate Buffer
    mbox[25] = 0x40001; mbox[26] = 8; mbox[27] = 8; mbox[28] = 4096; mbox[29] = 0;

    // 7. Get Pitch
    mbox[30] = 0x40008; mbox[31] = 4; mbox[32] = 4; mbox[33] = 0;

    mbox[34] = 0;

    if (mbox_call(8) && mbox[20] == 32 && mbox[28] != 0) {
        // Enmascaramos con 0x3FFFFFFF para convertir Dir. Bus a Dir. Física ARM
        mbox[28] &= 0x3FFFFFFF; 
        lfb = (unsigned char*)(unsigned long)mbox[28];
        width = mbox[5];
        height = mbox[6];
        pitch = mbox[33]; 

        uart_puts("[VIDEO] FB="); uart_hex(mbox[28]);
        uart_puts(" pitch="); uart_dec(pitch);
        if (pitch != 2560) uart_puts(" WARN:pitch!=2560");
        uart_puts("\n");
    } else {
        uart_puts("[VIDEO] ERROR: GPU rejected mailbox\n");
    }
}

void draw_pixel(int x, int y, uint32_t color) {
    if (!lfb || x < 0 || x >= (int)width || y < 0 || y >= (int)height) return;
    volatile uint32_t* ptr = (volatile uint32_t*)(lfb + (y * pitch) + (x * 4));
    *ptr = color;
}

void draw_fill(uint32_t color) {
    if (!lfb) return;
    for (int y = 0; y < (int)height; y++) {
        volatile uint32_t* row = (volatile uint32_t*)(lfb + (y * pitch));
        for (int x = 0; x < (int)width; x++) row[x] = color;
    }
}

void draw_rect(int x, int y, int w, int h, uint32_t color, int thickness) {
    for (int t = 0; t < thickness; t++) {
        for (int i = x; i < x + w; i++) { draw_pixel(i, y + t, color); draw_pixel(i, y + h - t, color); }
        for (int j = y; j < y + h; j++) { draw_pixel(x + t, j, color); draw_pixel(x + w - t, j, color); }
    }
}

void draw_tensor_image(const float* img, int x_off, int y_off, int img_w, int img_h) {
    for (int y = 0; y < img_h; y++) {
        for (int x = 0; x < img_w; x++) {
            int idx = y * img_w + x;
            
            // Leemos los canales originales del Tensor (RGB)
            int r = (int)(img[idx] * 255.0f);
            int g = (int)(img[img_w * img_h + idx] * 255.0f);
            int b = (int)(img[2 * img_w * img_h + idx] * 255.0f);
            
            // Clamp para seguridad
            if (r<0) r=0; else if (r>255) r=255;
            if (g<0) g=0; else if (g>255) g=255;
            if (b<0) b=0; else if (b>255) b=255;

            // CORRECCIÓN BGR:
            // Antes: (r << 16) | (g << 8) | b
            // Ahora: (b << 16) | (g << 8) | r
            // Ponemos el AZUL donde antes iba el ROJO para compensar a la GPU.
            
            uint32_t color = (0xFF << 24) | (b << 16) | (g << 8) | r; 
            
            draw_pixel(x_off + x, y_off + y, color);
        }
    }
}

void video_flush() { if (lfb) flush_to_ram((volatile void*)lfb, pitch * height); }

// ---- Bitmap font 5×7 (row-major, bit4=left, bit0=right) ----
// Supported charset (index matches position in k_font_chars):
static const char k_font_chars[] = "0123456789FPS:. ";

static const uint8_t k_font[16][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 1
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},  // 2
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},  // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},  // 4
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},  // 5
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},  // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},  // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},  // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},  // 9
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},  // F
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},  // P
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},  // S
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00},  // :
    {0x00,0x00,0x00,0x00,0x00,0x04,0x04},  // .
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // ' '
};

static int font_index(char c) {
    for (int i = 0; k_font_chars[i]; i++)
        if (k_font_chars[i] == c) return i;
    return 15;  // unknown → space
}

// Draw one character. Each glyph pixel becomes scale×scale screen pixels.
// fg = foreground color, bg = background color (format 0xAABBGGRR).
static void draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    const uint8_t* glyph = k_font[font_index(c)];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            uint32_t color = (bits & (0x10u >> col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    draw_pixel(x + col * scale + sx, y + row * scale + sy, color);
        }
    }
}

// Draw a null-terminated string. Characters are spaced (5+1)*scale pixels apart.
// Only characters present in k_font_chars are rendered; others become spaces.
void draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale) {
    int cx = x;
    while (*s) {
        draw_char(cx, y, *s++, fg, bg, scale);
        cx += (5 + 1) * scale;
    }
}