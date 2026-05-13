#pragma once

#include <stddef.h>
#include <stdint.h>

#include <config.h>

namespace tori::arch::x86_64 {

// GDT selector indices (fixed, shared across all CPUs)
enum GDTIndex : uint16_t {
    GDT_NULL          = 0,
    GDT_KERNEL_CODE   = 1,
    GDT_KERNEL_DATA   = 2,
    GDT_USER_DATA_32  = 3,   // Base for SYSRET
    GDT_USER_DATA     = 4,
    GDT_USER_CODE     = 5,
    GDT_TSS_FIRST     = 6,   // + 2 per CPU for TSS descriptor
};

inline constexpr uint16_t gdt_selector(GDTIndex idx) {
    return static_cast<uint16_t>(idx * 8);
}

// x86_64 TSS
struct TSS {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

// Total GDT entries: 6 fixed + 2 per CPU (TSS descriptor is 16 bytes)
inline constexpr size_t gdt_total_entries = 6 + 2 * CONFIG_MAX_CPUS;

void init_gdt();

// Loads the TSS for the specified CPU index into the TR register.
// Must be called after init_gdt().
void load_tss(size_t cpu_index);

} // namespace tori::arch::x86_64
