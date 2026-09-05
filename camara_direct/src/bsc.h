#pragma once
#include <stdint.h>

void bsc_init();
bool bsc_write(uint8_t addr, uint8_t* data, uint32_t len);
bool bsc_read(uint8_t addr, uint8_t* data, uint32_t len);