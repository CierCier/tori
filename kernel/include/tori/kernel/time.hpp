#pragma once

#include <stdint.h>

namespace tori::time {

// Initialize the time subsystem. bsp_lapic_id is used to designate 
// which CPU is responsible for the global system tick.
void init(uint32_t bsp_lapic_id, uint32_t frequency_hz);

// Called by the timer ISR on every CPU.
// Only the designated BSP increments the global tick.
void tick(uint32_t current_lapic_id);

// Returns the system uptime in milliseconds.
uint64_t uptime_ms();

} // namespace tori::time
