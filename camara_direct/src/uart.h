#pragma once
#include <stdint.h>

void uart_init();
void uart_puts(const char* s);
void uart_putc(char c);
void uart_hex(uint32_t d);
void uart_dec(uint32_t d);
