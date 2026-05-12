#include <tori/kernel/gdt.hpp>

#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/address.hpp>

namespace {

using namespace tori::arch::x86_64;

// GDT entry (8 bytes)
struct [[gnu::packed]] GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
};

// High 8 bytes of a system segment descriptor (TSS on x86_64)
struct [[gnu::packed]] GDTEntryHigh {
    uint32_t base_upper;
    uint32_t reserved;
};

// GDTR descriptor for lgdt
struct [[gnu::packed]] GDTR {
    uint16_t limit;
    uint64_t base;
};

// Per-CPU data
struct PerCPU {
    TSS tss;
    uint8_t stack[CONFIG_KERNEL_STACK_SIZE];
    uint8_t df_stack[CONFIG_DOUBLE_FAULT_STACK_SIZE];
};

// GDT entries + per-CPU data, all statically allocated
alignas(16) GDTEntry gdt[gdt_total_entries] = {};
PerCPU percpu[CONFIG_MAX_CPUS] = {};

GDTEntry make_gdt_entry(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    GDTEntry e = {};
    e.limit_low = limit & 0xFFFF;
    e.base_low = base & 0xFFFF;
    e.base_mid = (base >> 16) & 0xFF;
    e.access = access;
    e.flags_limit_high = ((flags & 0xF) << 4) | ((limit >> 16) & 0xF);
    e.base_high = (base >> 24) & 0xFF;
    return e;
}

void write_tss_descriptor(size_t gdt_index, uint64_t tss_base) {
    // First 8 bytes
    gdt[gdt_index].limit_low = sizeof(TSS) - 1;
    gdt[gdt_index].base_low = tss_base & 0xFFFF;
    gdt[gdt_index].base_mid = (tss_base >> 16) & 0xFF;
    gdt[gdt_index].access = 0x89;  // present, DPL=0, TSS64-available
    gdt[gdt_index].flags_limit_high = 0x00;
    gdt[gdt_index].base_high = (tss_base >> 24) & 0xFF;

    // Next 8 bytes (high part of descriptor)
    auto& high = reinterpret_cast<GDTEntryHigh&>(gdt[gdt_index + 1]);
    high.base_upper = static_cast<uint32_t>(tss_base >> 32);
    high.reserved = 0;
}

void init_tss(size_t cpu_index) {
    PerCPU& cpu = percpu[cpu_index];
    TSS& tss = cpu.tss;

    // Clear TSS
    for (size_t i = 0; i < sizeof(TSS) / sizeof(uint64_t); ++i) {
        reinterpret_cast<uint64_t*>(&tss)[i] = 0;
    }

    // Set RSP0 (kernel stack for ring 0 entry)
    tss.rsp[0] = reinterpret_cast<uint64_t>(cpu.stack) + CONFIG_KERNEL_STACK_SIZE;

    // Set IST1 for double faults
    tss.ist[0] = reinterpret_cast<uint64_t>(cpu.df_stack) + CONFIG_DOUBLE_FAULT_STACK_SIZE;

    // Write TSS descriptor into GDT
    const size_t tss_gdt_index = GDT_TSS_FIRST + cpu_index * 2;
    write_tss_descriptor(tss_gdt_index, reinterpret_cast<uint64_t>(&tss));
}

} // namespace

namespace tori::arch::x86_64 {

void init_gdt() {
    // Clear entire GDT
    for (size_t i = 0; i < gdt_total_entries; ++i) {
        reinterpret_cast<uint64_t*>(gdt)[i] = 0;
    }

    // Null descriptor at index 0 (already zero)

    // Kernel code: ring 0, 64-bit
    gdt[GDT_KERNEL_CODE] = make_gdt_entry(0, 0, 0x9A, 0x2);

    // Kernel data: ring 0
    gdt[GDT_KERNEL_DATA] = make_gdt_entry(0, 0, 0x92, 0x0);

    // User code: ring 3, 64-bit
    gdt[GDT_USER_CODE] = make_gdt_entry(0, 0, 0xFA, 0x2);

    // User data: ring 3
    gdt[GDT_USER_DATA] = make_gdt_entry(0, 0, 0xF2, 0x0);

    // Initialize per-CPU TSS blocks
    for (size_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
        init_tss(cpu);
    }

    // Load GDT
    GDTR gdtr = {};
    gdtr.limit = static_cast<uint16_t>(gdt_total_entries * sizeof(GDTEntry) - 1);
    gdtr.base = reinterpret_cast<uint64_t>(gdt);

    // Load GDT and update segment registers
    asm volatile(
        "lgdt %0\n"
        "push %1\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        "mov %2, %%ds\n"
        "mov %2, %%es\n"
        "mov %2, %%fs\n"
        "mov %2, %%gs\n"
        "mov %2, %%ss\n"
        :
        : "m"(gdtr),
          "ri"(static_cast<uint64_t>(gdt_selector(GDT_KERNEL_CODE))),
          "r"(static_cast<uint16_t>(gdt_selector(GDT_KERNEL_DATA)))
        : "rax", "memory"
    );

    // Load TSS for CPU 0
    const size_t tss_gdt_idx = GDT_TSS_FIRST + 0 * 2;
    const uint16_t tss_selector = static_cast<uint16_t>(tss_gdt_idx * 8);
    asm volatile("ltr %0" : : "r"(tss_selector) : "memory");

    TORI_LOG_INFO("gdt", "GDT and TSS initialized");
}

} // namespace tori::arch::x86_64
