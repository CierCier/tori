#include <tori/kernel/address.hpp>

namespace {

uint64_t active_hhdm_offset = 0;

} // namespace

namespace tori::memory::address {

void init_hhdm(uint64_t hhdm_offset) {
    active_hhdm_offset = hhdm_offset;
}

uint64_t hhdm_offset() {
    return active_hhdm_offset;
}

void* physical_to_virtual(uint64_t physical_address) {
    return reinterpret_cast<void*>(physical_address + active_hhdm_offset);
}

uint64_t virtual_to_physical(const void* virtual_address) {
    return reinterpret_cast<uint64_t>(virtual_address) - active_hhdm_offset;
}

} // namespace tori::memory::address
