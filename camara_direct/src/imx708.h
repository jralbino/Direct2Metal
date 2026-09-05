#pragma once
#include <stdint.h>

void imx708_init_baseline();
bool imx708_probe();
void imx708_stream_on();
uint8_t imx708_read_frame_count();
void imx708_write(uint16_t reg, uint8_t val);
uint8_t imx708_read(uint16_t reg);
