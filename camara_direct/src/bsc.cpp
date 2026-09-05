#include "bsc.h"
#include "uart.h"
#include <stdint.h>

// Base correcta para el I2C de la cámara (BSC1)
#define BSC_BASE   0x3F804000
#define BSC_C       (*(volatile uint32_t*)(BSC_BASE + 0x00))
#define BSC_S       (*(volatile uint32_t*)(BSC_BASE + 0x04))
#define BSC_DLEN    (*(volatile uint32_t*)(BSC_BASE + 0x08))
#define BSC_A       (*(volatile uint32_t*)(BSC_BASE + 0x0C))
#define BSC_FIFO    (*(volatile uint32_t*)(BSC_BASE + 0x10))
#define BSC_DIV     (*(volatile uint32_t*)(BSC_BASE + 0x14))

#define GPIO_BASE   0x3F200000
#define GPFSEL4     (*(volatile uint32_t*)(GPIO_BASE + 0x10))
#define GPSET1      (*(volatile uint32_t*)(GPIO_BASE + 0x20))
#define GPCLR1      (*(volatile uint32_t*)(GPIO_BASE + 0x2C))
#define GPPUD       (*(volatile uint32_t*)(GPIO_BASE + 0x94))
#define GPPUDCLK1   (*(volatile uint32_t*)(GPIO_BASE + 0x9C))

static void camera_gpio_override_high() {
    // Poner GPIO40/41/42/44 a salida.
    uint32_t sel = GPFSEL4;
    sel &= ~(7 << 0);   // Limpiar bits del GPIO 40
    sel |= (1 << 0);    // GPIO 40 = output
    sel &= ~(7 << 3);   // Limpiar bits del GPIO 41
    sel |= (1 << 3);    // GPIO 41 = output
    sel &= ~(7 << 6);   // Limpiar bits del GPIO 42
    sel |= (1 << 6);    // GPIO 42 = output
    sel &= ~(7 << 12);  // Limpiar bits del GPIO 44
    sel |= (1 << 12);   // GPIO 44 = output
    GPFSEL4 = sel;

    /* Hard reset pulse: drive XSHUTDOWN/regulator LOW (kills sensor power +
     * latches it in reset), wait for caps to discharge + sensor to release
     * any internal state, then HIGH to bring it back online. Recovers from
     * "sensor stuck mid-stream" states left by a previous run that didn't
     * clean up — symptom is persistent NACK on the I2C probe even with the
     * cable reseated. */
    uint32_t pins = (1u << (40 - 32)) | (1u << (41 - 32))
                  | (1u << (42 - 32)) | (1u << (44 - 32));

    GPCLR1 = pins;                                 // pull LOW
    for (volatile int i = 0; i < 30000000; i++);   // ~50 ms discharge

    GPSET1 = pins;                                 // release HIGH
    for (volatile int i = 0; i < 6000000; i++);    // ~10 ms boot
}

void bsc_init() {
    // Forzar alta en las señales de control de la cámara antes de usar I2C.
    camera_gpio_override_high();

    // Configurar GPIO 44 (SDA1) y GPIO 45 (SCL1) a la función ALT2.
    // El valor para ALT2 en el registro GPFSEL es 6 (binario 110).
    uint32_t sel = GPFSEL4;
    sel &= ~(7 << 12); // Limpiar bits del GPIO 44
    sel |= (6 << 12);  // 6 = ALT2
    sel &= ~(7 << 15); // Limpiar bits del GPIO 45
    sel |= (6 << 15);  // 6 = ALT2
    GPFSEL4 = sel;

    // Activar pull-ups internos en GPIO44/45 para asegurar I2C estable.
    GPPUD = 2; // Pull-up
    for (volatile int i = 0; i < 150; i++);
    GPPUDCLK1 = (1 << (44 - 32)) | (1 << (45 - 32));
    for (volatile int i = 0; i < 150; i++);
    GPPUD = 0;
    GPPUDCLK1 = 0;

    // Configurar Baudrate (~100kHz)
    BSC_DIV = 2500;
}

bool bsc_write(uint8_t addr, uint8_t* data, uint32_t len) {
    BSC_A = addr;
    BSC_DLEN = len;
    BSC_S = 0x302; // Limpiar flags
    BSC_C = 0x8030; // I2CEN | CLEAR FIFO/status

    for(uint32_t i = 0; i < len; i++) BSC_FIFO = data[i];

    BSC_C = 0x8080; // I2CEN | ST
    uint32_t timeout = 1000000;
    while (!(BSC_S & 0x02) && --timeout);
    if (!timeout) {
        uart_puts("BSC: write timeout waiting DONE\n");
        return false;
    }
    if (BSC_S & 0x100) {
        uart_puts("BSC: write NACK status=0x"); uart_hex(BSC_S); uart_puts("\n");
        return false;
    }
    return true;
}

bool bsc_read(uint8_t addr, uint8_t* data, uint32_t len) {
    BSC_A = addr;
    BSC_DLEN = len;
    BSC_S = 0x302; // Limpiar flags
    BSC_C = 0x8030; // I2CEN | CLEAR FIFO/status

    BSC_C = 0x8081; // I2CEN | ST | READ
    uint32_t timeout = 1000000;
    while (!(BSC_S & 0x02) && --timeout);
    if (!timeout) {
        uart_puts("BSC: read timeout waiting DONE\n");
        return false;
    }
    if (BSC_S & 0x100) {
        uart_puts("BSC: read NACK status=0x"); uart_hex(BSC_S); uart_puts("\n");
        return false;
    }

    for(uint32_t i = 0; i < len; i++) data[i] = BSC_FIFO;
    return true;
}