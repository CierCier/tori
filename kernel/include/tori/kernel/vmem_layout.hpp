#pragma once

#include <stdint.h>

namespace tori::memory::vmem {

// x86_64 48-bit virtual address space layout:
//
//   0x0000000000000000 - 0x00007FFFFFFFFFFF   User space (future)
//   0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF   Kernel space
//
// Within kernel space:
//   Kernel image     at __kernel_virtual_start (0xFFFFFFFF80000000)
//   HHDM map         at offset set by bootloader
//   Heap / vmalloc   dynamic ranges (reserved for future VMM)

constexpr uint64_t user_base    = 0x0000000000000000;
constexpr uint64_t user_limit   = 0x00007FFFFFFFFFFF;
constexpr uint64_t kernel_base  = 0xFFFF800000000000;
constexpr uint64_t kernel_limit = 0xFFFFFFFFFFFFFFFF;

// Kernel image is linked at 0xFFFFFFFF80000000 by linker.ld.
// These are set by the linker and exported via symbols:
extern "C" char __kernel_virtual_start[];
extern "C" char __kernel_virtual_end[];

inline uint64_t kernel_image_start() {
    return reinterpret_cast<uint64_t>(__kernel_virtual_start);
}

inline uint64_t kernel_image_size() {
    const uint64_t start = reinterpret_cast<uint64_t>(__kernel_virtual_start);
    const uint64_t end = reinterpret_cast<uint64_t>(__kernel_virtual_end);
    return end > start ? end - start : 0;
}

inline uint64_t kernel_image_end() {
    return kernel_image_start() + kernel_image_size();
}

// Simple virtual range descriptor
struct Range {
    uint64_t base;
    uint64_t size;
};

inline bool range_contains(const Range& r, uint64_t address) {
    return address >= r.base && address < r.base + r.size;
}

inline bool ranges_overlap(const Range& a, const Range& b) {
    return a.base < b.base + b.size && b.base < a.base + a.size;
}

// Returns true if address is in the higher-half kernel space
inline bool is_kernel_address(uint64_t address) {
    return address >= kernel_base && address <= kernel_limit;
}

// Returns true if address is in the lower-half user space
inline bool is_user_address(uint64_t address) {
    return address <= user_limit;
}

} // namespace tori::memory::vmem
