/* File: src/kernel.cpp */
#include <stdint.h>

/* ... (Keep existing UART and helper functions same as before) ... */
volatile uint32_t* const UART0_DR = (uint32_t*)0x3F201000;
void uart_putc(char c) { *UART0_DR = (unsigned int)c; }
void uart_puts(const char* str) { while (*str) uart_putc(*str++); }
void uart_hex(uint64_t n) {
    const char* hex_chars = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc(hex_chars[(n >> i) & 0xF]);
    uart_puts("\n");
}
uint64_t get_system_timer() {
    uint64_t cnt;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

/* --- BENCHMARKS --- */

#define N 16
// Arrays global para no explotar el stack
int A[N][N], B[N][N], C_cpp[N][N], C_neon[N][N];

void matmul_cpp() {
    for(int i=0; i<N; i++)
        for(int j=0; j<N; j++)
            for(int k=0; k<N; k++)
                C_cpp[i][j] += A[i][k] * B[k][j];
}

// Declaramos la función externa de ensamblador
extern "C" void matmul_neon(int* A, int* B, int* C, int n);

extern "C" void kernel_main() {
    uart_puts("--- DIRECT-TO-METAL BENCHMARK ---\n");

    /* Initialize Data */
    for(int i=0; i<N; i++) {
        for(int j=0; j<N; j++) {
            A[i][j] = 1; // Simple data
            B[i][j] = 1;
            C_cpp[i][j] = 0;
            C_neon[i][j] = 0;
        }
    }

    /* TEST 1: C++ -O3 */
    uint64_t t1 = get_system_timer();
    matmul_cpp();
    uint64_t t2 = get_system_timer();
    uart_puts("C++ Cycles: ");
    uart_hex(t2 - t1);

    /* TEST 2: NEON ASSEMBLY */
    uint64_t t3 = get_system_timer();
    // Pasamos los punteros al inicio de los arrays (cast a int*)
    matmul_neon((int*)A, (int*)B, (int*)C_neon, N);
    uint64_t t4 = get_system_timer();
    
    uart_puts("NEON Cycles: ");
    uart_hex(t4 - t3);

    /* Validation (Optional but recommended) */
    if (C_cpp[0][0] == C_neon[0][0]) {
        uart_puts("Result Match: YES\n");
    } else {
        uart_puts("Result Match: NO (Bug in ASM)\n");
    }
}