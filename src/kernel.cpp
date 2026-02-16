/* File: src/kernel.cpp (Optimized Pointers) */
#include <stdint.h>
#include "model_config.h" 

/* UART Utils */
volatile uint32_t* const UART0_DR = (uint32_t*)0x3F201000;
void uart_putc(char c) { *UART0_DR = (unsigned int)c; }
void uart_puts(const char* str) { while (*str) uart_putc(*str++); }
void uart_hex(uint64_t n) {
    const char* hex = "0123456789ABCDEF";
    uart_puts("0x");
    for(int i=60; i>=0; i-=4) uart_putc(hex[(n>>i)&0xF]);
    uart_puts("\n");
}

extern "C" {
    extern const float weights_start[];
    /* Nueva firma: acepta stride (ancho en bytes) */
    extern void neon_kernel_3x3(float* input_ptr, float* weights, float* output, int input_stride_bytes);
}

#define IMG_H 16
#define IMG_W 16
float input_image[L1_IN_CH * IMG_H * IMG_W];
float output_map[L1_OUT_CH * IMG_H * IMG_W];
float weights_opt[L1_OUT_CH * L1_IN_CH * L1_KER_SZ * L1_KER_SZ];

void optimize_weights_grouped() {
    int count = 0;
    int kernel_vol = 27; 
    for (int out_g = 0; out_g < L1_OUT_CH; out_g += 4) {
        for (int k = 0; k < kernel_vol; k++) {
            weights_opt[count++] = weights_start[(out_g + 0)*kernel_vol + k];
            weights_opt[count++] = weights_start[(out_g + 1)*kernel_vol + k];
            weights_opt[count++] = weights_start[(out_g + 2)*kernel_vol + k];
            weights_opt[count++] = weights_start[(out_g + 3)*kernel_vol + k];
        }
    }
}

void conv2d_neon_fast() {
    float result_4[4];
    
    // Stride = Ancho * sizeof(float)
    int stride_bytes = IMG_W * 4;

    /* Evitamos los bordes (1 pixel) para no lidiar con Padding hoy */
    /* Inferencia valida solo de 1 a 15 */
    for (int h = 1; h < IMG_H - 1; h++) {
        for (int w = 1; w < IMG_W - 1; w++) {
            
            // Calculamos puntero al pixel superior-izquierdo del patch (h-1, w-1)
            // Solo necesitamos la dirección del Canal 0, el ASM saltará a los otros canales.
            float* in_ptr = &input_image[((h-1) * IMG_W) + (w-1)];

            for (int out_g = 0; out_g < L1_OUT_CH; out_g += 4) {
                float* w_ptr = &weights_opt[ (out_g / 4) * 108 ];
                
                // LLAMADA DIRECTA (Sin copia temporal)
                neon_kernel_3x3(in_ptr, w_ptr, result_4, stride_bytes);
                
                for(int i=0; i<4; i++) {
                    int out_idx = ((out_g + i) * IMG_H * IMG_W) + (h * IMG_W) + w;
                    output_map[out_idx] = result_4[i];
                }
            }
        }
    }
}

extern "C" void kernel_main() {
    uart_puts("\n=== NEON FAST (NO-COPY) TEST ===\n");

    for (int i = 0; i < L1_IN_CH * IMG_H * IMG_W; i++) 
        input_image[i] = (float)(i % 255) / 255.0f;

    optimize_weights_grouped();

    uart_puts("Ejecutando Conv2d Fast...\n");
    
    uint64_t start, end;
    asm volatile("mrs %0, cntpct_el0" : "=r"(start));
    
    conv2d_neon_fast();
    
    asm volatile("mrs %0, cntpct_el0" : "=r"(end));

    uart_puts("Ciclos NEON Fast: "); 
    uart_hex(end - start);
    
    uart_puts("Referencia C++: ~0x163FF\n");
    uart_puts("Referencia NEON Lento: ~0x2DA40\n");

    while(1) { asm volatile("wfe"); }
}