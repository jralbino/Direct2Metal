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
        *((volatile uint32_t*)0x3F201000) = n; 
        while (*((volatile uint32_t*)0x3F201018) & (1 << 5)); 
    }
}

uint32_t width, height, pitch, isrgb;
unsigned char* lfb = 0; 

void video_init() {
    uart_puts("\n--- INICIANDO PROTOCOLO HDMI (640x480) ---\n");

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

        uart_puts("EXITO! Framebuffer mapeado en: "); uart_hex(mbox[28]); uart_puts("\n");
        uart_puts("Pitch real: "); uart_dec(pitch); uart_puts(" bytes.\n");
    } else {
        uart_puts("ERROR FATAL: La GPU rechazo el mensaje.\n");
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