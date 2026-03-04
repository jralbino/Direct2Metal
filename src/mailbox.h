/* File: src/mailbox.h - VIDEOCORE MAILBOX INTERFACE */
#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

// Direcciones Base (Para Raspberry Pi Zero 2 W / BCM2837)
#define PERIPHERAL_BASE 0x3F000000
#define MAILBOX_BASE    (PERIPHERAL_BASE + 0xB880)

// Registros del Mailbox
#define MAILBOX_READ    ((volatile uint32_t*)(MAILBOX_BASE + 0x00))
#define MAILBOX_STATUS  ((volatile uint32_t*)(MAILBOX_BASE + 0x18))
#define MAILBOX_WRITE   ((volatile uint32_t*)(MAILBOX_BASE + 0x20))

// Constantes de Estado
#define MAILBOX_EMPTY   0x40000000
#define MAILBOX_FULL    0x80000000

// Canales
#define MBOX_CH_PROP    8  // Canal 8: Propiedades ARM -> VC

// Buffer del Mailbox (Debe estar alineado a 16 bytes en RAM)
extern volatile uint32_t mbox[36];

// Función principal
int mbox_call(unsigned char ch); // <--- CAMBIO AQUÍ

#endif