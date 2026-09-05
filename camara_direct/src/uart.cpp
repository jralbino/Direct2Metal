#include "uart.h"

#define MMIO_BASE       0x3F000000
#define GPIO_BASE       (MMIO_BASE + 0x200000)
#define UART0_BASE      (MMIO_BASE + 0x201000)

// Registros GPIO
#define GPFSEL1         (*(volatile uint32_t*)(GPIO_BASE + 0x04))
#define GPPUD           (*(volatile uint32_t*)(GPIO_BASE + 0x94))
#define GPPUDCLK0       (*(volatile uint32_t*)(GPIO_BASE + 0x98))

// Registros UART0 (PL011)
#define UART0_DR        (*(volatile uint32_t*)(UART0_BASE + 0x00))
#define UART0_FR        (*(volatile uint32_t*)(UART0_BASE + 0x18))
#define UART0_IBRD      (*(volatile uint32_t*)(UART0_BASE + 0x24))
#define UART0_FBRD      (*(volatile uint32_t*)(UART0_BASE + 0x28))
#define UART0_LCRH      (*(volatile uint32_t*)(UART0_BASE + 0x2C))
#define UART0_CR        (*(volatile uint32_t*)(UART0_BASE + 0x30))
#define UART0_ICR       (*(volatile uint32_t*)(UART0_BASE + 0x44))

void uart_init() {
    // 1. Deshabilitar UART
    UART0_CR = 0;

    // 2. Configurar GPIO 14 y 15 a ALT0
    uint32_t selector = GPFSEL1;
    selector &= ~(7 << 12); // Limpiar GPIO 14
    selector |= (4 << 12);  // ALT0 para GPIO 14 (TXD0)
    selector &= ~(7 << 15); // Limpiar GPIO 15
    selector |= (4 << 15);  // ALT0 para GPIO 15 (RXD0)
    GPFSEL1 = selector;

    // 3. Deshabilitar pull-up/down para los pines 14 y 15
    GPPUD = 0;
    for(volatile int i=0; i<150; i++);
    GPPUDCLK0 = (1 << 14) | (1 << 15);
    for(volatile int i=0; i<150; i++);
    GPPUDCLK0 = 0;

    // 4. Limpiar interrupciones
    UART0_ICR = 0x7FF;

    // 5. Configurar Baudrate (115200)
    // Para 48MHz: 48000000 / (16 * 115200) = 26.04166
    // Integer = 26, Fractional = (0.04166 * 64 + 0.5) = 3
    UART0_IBRD = 26;
    UART0_FBRD = 3;

    // 6. 8 bits, FIFO habilitado, 1 stop bit, sin paridad
    UART0_LCRH = (3 << 5) | (1 << 4);

    // 7. Habilitar UART, TX y RX
    UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putc(char c) {
    while (UART0_FR & (1 << 5)); // Esperar si el transmisor está lleno
    UART0_DR = c;
}

void uart_hex(uint32_t d) {
    char n;
    for(int i=28; i>=0; i-=4) {
        n = (d >> i) & 0xF;
        n += (n > 9) ? 0x37 : 0x30;
        uart_putc(n);
    }
}

void uart_puts(const char* s) {
    while (*s) {
        if(*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_dec(uint32_t d) {
    if (d == 0) { uart_putc('0'); return; }
    char buf[10];
    int n = 0;
    while (d) { buf[n++] = '0' + (d % 10); d /= 10; }
    while (n--) uart_putc(buf[n]);
}
