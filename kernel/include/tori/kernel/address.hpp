#pragma once

#include <stdint.h>

namespace tori::memory::address {

void init_hhdm(uint64_t hhdm_offset);
uint64_t hhdm_offset();
void* physical_to_virtual(uint64_t physical_address);
uint64_t virtual_to_physical(const void* virtual_address);

} // namespace tori::memory::address
