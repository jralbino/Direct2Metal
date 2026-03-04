/* File: src/camera_bsc.cpp - V17: OMNI-CLOCK RESTORED */
#include <stdint.h>

extern void uart_puts(const char* s);
extern void uart_dec(int n);

#define GPFSEL0    ((volatile uint32_t*)0x3F200000)
#define GPFSEL3    ((volatile uint32_t*)0x3F20000C) 
#define GPFSEL4    ((volatile uint32_t*)0x3F200010) 
#define GPSET1     ((volatile uint32_t*)0x3F200020) 
#define GPCLR1     ((volatile uint32_t*)0x3F20002C) 
#define CM_GP0CTL  ((volatile uint32_t*)0x3F101070)
#define CM_GP0DIV  ((volatile uint32_t*)0x3F101074)
// GPCLK2 — este es el MCLK real del sensor en Pi Zero 2W (GPIO43 = ALT0 = GPCLK2)
// dtoverlay=imx708 lo configura a 24MHz desde XOSC o PLLD.
#define CM_GP2CTL  ((volatile uint32_t*)0x3F101080)
#define CM_GP2DIV  ((volatile uint32_t*)0x3F101084)
#define CM_PASSWD  0x5A000000u

#define BSC0_BASE  0x3F205000UL
#define BSC1_BASE  0x3F804000UL

static volatile uint32_t* BSC_C = 0;
static volatile uint32_t* BSC_S = 0;
static volatile uint32_t* BSC_DLEN = 0;
static volatile uint32_t* BSC_A = 0;
static volatile uint32_t* BSC_FIFO = 0;
static volatile uint32_t* BSC_DIV = 0;

#define BSC_C_I2CEN  (1u << 15)
#define BSC_C_ST     (1u <<  7)
#define BSC_C_CLEAR  (1u <<  4)
#define BSC_C_READ   (1u <<  0)
#define BSC_S_DONE   (1u << 1)
#define BSC_S_ERR    (1u << 8)

static void delay_ms(int ms) {
    for(volatile int i = 0; i < 100000 * ms; i++) { asm volatile("nop"); }
}

void select_bus(int bus_id) {
    uint32_t base = (bus_id == 0) ? BSC0_BASE : BSC1_BASE;
    BSC_C    = (volatile uint32_t*)(base + 0x00);
    BSC_S    = (volatile uint32_t*)(base + 0x04);
    BSC_DLEN = (volatile uint32_t*)(base + 0x08);
    BSC_A    = (volatile uint32_t*)(base + 0x0C);
    BSC_FIFO = (volatile uint32_t*)(base + 0x10);
    BSC_DIV  = (volatile uint32_t*)(base + 0x14);

    if (bus_id == 0) {
        uint32_t fsel0 = *GPFSEL0;
        fsel0 &= ~((7u << 0) | (7u << 3));
        fsel0 |=  ((4u << 0) | (4u << 3));
        *GPFSEL0 = fsel0;
    } else {
        uint32_t fsel4 = *GPFSEL4;
        fsel4 &= ~((7u << 12) | (7u << 15));
        fsel4 |=  ((6u << 12) | (6u << 15)); 
        *GPFSEL4 = fsel4;
    }
    *BSC_DIV = 5000; 
    *BSC_C   = BSC_C_I2CEN | BSC_C_CLEAR;
}

static bool wait_done() {
    for (int i = 0; i < 50000; i++) {
        if (*BSC_S & BSC_S_DONE) return true;
        if (*BSC_S & BSC_S_ERR)  { *BSC_S = BSC_S_ERR; return false; }
    }
    return false;
}

bool probe_address(uint8_t addr) {
    *BSC_C = BSC_C_I2CEN | BSC_C_CLEAR; *BSC_S = 0x302;
    *BSC_A = addr; *BSC_DLEN = 0;
    *BSC_C = BSC_C_I2CEN | BSC_C_ST;
    return wait_done();
}

void bsc_init_and_find() {
    uart_puts("[CAM] Iniciando Secuencia de Hard-Reset...\n");

    uint32_t fsel4 = *GPFSEL4;
    fsel4 &= ~((7u << 0) | (7u << 3) | (7u << 6) | (7u << 12));
    fsel4 |=  ((1u << 0) | (1u << 3) | (1u << 6) | (1u << 12)); 
    *GPFSEL4 = fsel4;

    *GPCLR1 = (1u << (40-32)) | (1u << (41-32)) | (1u << (42-32)) | (1u << (44-32));
    delay_ms(200);

    // ── MCLK ANTES DE XSHUTDOWN ─────────────────────────────────────────────────
    // V23 diagnóstico: dtoverlay=imx708 configura CM_GP2 con SRC=XOSC (19.2MHz),
    // DIVI=585, DIVF=3840 → ~32.6 kHz. El IMX708 necesita 24 MHz ± 5%.
    // Con 32.6 kHz su PHY MIPI no puede lock → nunca emite MIPI aunque I2C funcione.
    //
    // FIX V24: reconfiguramos CM_GP2 a SRC=PLLD (500MHz), DIVI=20 → 25MHz (dentro ±5%).
    // DEBE hacerse ANTES de que XSHUTDOWN suba (GPSET1) para que el sensor arranque
    // con MCLK correcto desde el primer ciclo interno.
    uart_puts("      > Configurando GPCLK2 (25MHz, SRC=PLLD) en GPIO43...\n");
    {
        // Detener CM_GP2 antes de cambiar divisor (requisito BCM2835 clock manager).
        *CM_GP2CTL = CM_PASSWD | 6u;  // ENAB=0, SRC=PLLD (pre-seleccionar fuente)
        for (volatile int i = 0; i < 100000; i++) {
            if (!(*CM_GP2CTL & (1u << 7))) break;  // esperar BUSY=0
            asm volatile("nop");
        }
        // DIVI=20, DIVF=853, MASH=1 → 500MHz / (20 + 853/1024) ≈ 24.0 MHz
        // Si MASH=1 write falla (readback MASH=0): 500/20 = 25 MHz — también dentro ±5%.
        *CM_GP2DIV = CM_PASSWD | (20u << 12) | 853u;
        *CM_GP2CTL = CM_PASSWD | (1u << 9) | (1u << 4) | 6u;  // MASH=1, ENAB, SRC=PLLD
        delay_ms(2);  // PLL settle antes de XSHUTDOWN
        uart_puts("      > CM_GP2CTL="); uart_dec((int)*CM_GP2CTL);
        uart_puts(" CM_GP2DIV="); uart_dec((int)*CM_GP2DIV); uart_puts("\n");
    }
    // ────────────────────────────────────────────────────────────────────────────

    // Legacy GPCLK0 en GPIO32/34 (no conectados al sensor en Pi Zero 2W, inofensivo)
    *CM_GP0CTL = CM_PASSWD | 6;
    while ((*CM_GP0CTL) & (1<<7)) { asm volatile("nop"); }
    *CM_GP0DIV = CM_PASSWD | (20u << 12) | 3413u;
    *CM_GP0CTL = CM_PASSWD | (1u << 4) | 6u;

    uint32_t fsel3 = *GPFSEL3;
    fsel3 &= ~((7u << 6) | (7u << 12));
    fsel3 |=  ((2u << 6) | (4u << 12));
    *GPFSEL3 = fsel3;

    uint32_t fsel0 = *GPFSEL0;
    fsel0 &= ~(7u << 12);
    fsel0 |=  (4u << 12);
    *GPFSEL0 = fsel0;

    // ── XSHUTDOWN HIGH — sensor arranca con MCLK correcto (25MHz) ───────────────
    *GPSET1 = (1u << (40-32)) | (1u << (41-32)) | (1u << (42-32)) | (1u << (44-32));
    uart_puts("      > Pines 40/41/42/44 HIGH (Wake up con MCLK=25MHz)...\n");
    delay_ms(300);

    // Verificar estado final de GPIO43 y CM_GP2
    {
        uint32_t gpio43_fsel = (*GPFSEL4 >> 9) & 7u;
        uint32_t gp2ctl = *CM_GP2CTL;
        uart_puts("[CAM] GPIO43 FSEL="); uart_dec((int)gpio43_fsel);
        uart_puts(" CM_GP2CTL="); uart_dec((int)gp2ctl);
        uart_puts(gp2ctl & (1u<<7) ? " (BUSY=OK)\n" : " (BUSY=0 ERROR!)\n");
        if (gpio43_fsel != 4u) {
            uart_puts("[CAM] WARNING: GPIO43 no es GPCLK2! Forzando ALT0...\n");
            uint32_t fsel4_fix = *GPFSEL4;
            fsel4_fix &= ~(7u << 9);
            fsel4_fix |=  (4u << 9);
            *GPFSEL4 = fsel4_fix;
        }
    }

    uart_puts("[CAM] Probando Bus 1 (GPIO 44/45 via BSC1)...\n");
    select_bus(1);
    if (probe_address(0x1A)) {
        uart_puts("      > ENCONTRADO! Sensor en Bus 1.\n");
        return;
    }

    uart_puts("[CAM] ERROR: Sensor no encontrado.\n");
    select_bus(1);   
}

void i2c_write_reg16(uint8_t dev_addr, uint16_t reg, uint8_t val) {
    *BSC_C = BSC_C_I2CEN | BSC_C_CLEAR; *BSC_S = 0x302;
    *BSC_A = dev_addr; *BSC_DLEN = 3;
    *BSC_FIFO = (reg >> 8) & 0xFF; *BSC_FIFO = reg & 0xFF; *BSC_FIFO = val;
    *BSC_C = BSC_C_I2CEN | BSC_C_ST;
    wait_done();
}

uint8_t i2c_read_reg16(uint8_t dev_addr, uint16_t reg) {
    *BSC_C = BSC_C_I2CEN | BSC_C_CLEAR; *BSC_S = 0x302;
    *BSC_A = dev_addr; *BSC_DLEN = 2;
    *BSC_FIFO = (reg >> 8) & 0xFF; *BSC_FIFO = reg & 0xFF;
    *BSC_C = BSC_C_I2CEN | BSC_C_ST;
    if (!wait_done()) return 0xFF;

    *BSC_C = BSC_C_I2CEN | BSC_C_CLEAR; *BSC_S = 0x302;
    *BSC_A = dev_addr; *BSC_DLEN = 1;
    *BSC_C = BSC_C_I2CEN | BSC_C_ST | BSC_C_READ;
    if (!wait_done()) return 0xFF;
    return (uint8_t)(*BSC_FIFO & 0xFF);
}