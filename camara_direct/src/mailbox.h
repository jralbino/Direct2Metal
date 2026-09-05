#pragma once
#include <stdint.h>

bool mailbox_set_domain_state(uint32_t domain, uint32_t state);
bool mailbox_set_clock_rate(uint32_t clock_id, uint32_t rate);
