#include <tori/kernel/gdt.hpp>

#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/address.hpp>

namespace {

using namespace tori::arch::x86_64;

struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
};

struct GDTEntryHigh {
    uint32_t base_upper;
    uint32_t reserved;
};

struct GDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct PerCPU {
    uint64_t syscall_rsp;    // offset 0: kernel stack for syscall entry
    uint64_t user_rsp_save;  // offset 8: user RSP saved on syscall entry
    TSS tss;
    uint8_t stack[CONFIG_KERNEL_STACK_SIZE];
    uint8_t df_stack[CONFIG_DOUBLE_FAULT_STACK_SIZE];
};

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
    gdt[gdt_index].limit_low = sizeof(TSS) - 1;
    gdt[gdt_index].base_low = tss_base & 0xFFFF;
    gdt[gdt_index].base_mid = (tss_base >> 16) & 0xFF;
    gdt[gdt_index].access = 0x89;
    gdt[gdt_index].flags_limit_high = 0x00;
    gdt[gdt_index].base_high = (tss_base >> 24) & 0xFF;

    auto& high = reinterpret_cast<GDTEntryHigh&>(gdt[gdt_index + 1]);
    high.base_upper = static_cast<uint32_t>(tss_base >> 32);
    high.reserved = 0;
}

void init_tss(size_t cpu_index) {
    PerCPU& cpu = percpu[cpu_index];
    TSS& tss = cpu.tss;

    for (size_t i = 0; i < sizeof(TSS) / sizeof(uint64_t); ++i) {
        reinterpret_cast<uint64_t*>(&tss)[i] = 0;
    }

    tss.rsp[0] = reinterpret_cast<uint64_t>(cpu.stack) + CONFIG_KERNEL_STACK_SIZE;
    tss.ist[0] = reinterpret_cast<uint64_t>(cpu.df_stack) + CONFIG_DOUBLE_FAULT_STACK_SIZE;

    const size_t tss_gdt_index = GDT_TSS_FIRST + cpu_index * 2;
    write_tss_descriptor(tss_gdt_index, reinterpret_cast<uint64_t>(&tss));
}

void setup_percpu(size_t cpu_index) {
    PerCPU& cpu = percpu[cpu_index];
    cpu.syscall_rsp = reinterpret_cast<uint64_t>(cpu.stack) + CONFIG_KERNEL_STACK_SIZE;
}

void load_percpu_gs_base(size_t cpu_index) {
    uint64_t base = reinterpret_cast<uint64_t>(&percpu[cpu_index]);
    uint32_t lo = static_cast<uint32_t>(base);
    uint32_t hi = static_cast<uint32_t>(base >> 32);
    // Write MSR_KERNEL_GS_BASE (0xC0000102) so swapgs gives us per-CPU data.
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000102) : "memory");
}

} // namespace

namespace tori::arch::x86_64 {

void init_gdt() {
    static bool initialized = false;
    if (!initialized) {
        for (size_t i = 0; i < gdt_total_entries; ++i) {
            reinterpret_cast<uint64_t*>(gdt)[i] = 0;
        }

        gdt[GDT_KERNEL_CODE] = make_gdt_entry(0, 0, 0x9A, 0x2);
        gdt[GDT_KERNEL_DATA] = make_gdt_entry(0, 0, 0x92, 0x0);
        gdt[GDT_USER_DATA_32] = make_gdt_entry(0, 0, 0xF2, 0x0); // Dummy for SYSRET base
        gdt[GDT_USER_DATA] = make_gdt_entry(0, 0, 0xF2, 0x0);
        gdt[GDT_USER_CODE] = make_gdt_entry(0, 0, 0xFA, 0x2);

        for (size_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) {
            setup_percpu(cpu);
            init_tss(cpu);
        }
        initialized = true;
    }

    GDTR gdtr = {};
    gdtr.limit = static_cast<uint16_t>(gdt_total_entries * sizeof(GDTEntry) - 1);
    gdtr.base = reinterpret_cast<uint64_t>(gdt);

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
}

void load_tss(size_t cpu_index) {
    if (cpu_index >= CONFIG_MAX_CPUS) return;

    const size_t tss_gdt_idx = GDT_TSS_FIRST + cpu_index * 2;
    const uint16_t tss_selector = static_cast<uint16_t>(tss_gdt_idx * 8);
    asm volatile("ltr %0" : : "r"(tss_selector) : "memory");

    load_percpu_gs_base(cpu_index);

    TORI_LOG_INFO("gdt", "GDT and TSS loaded for CPU");
    TORI_LOG_VALUE(log::Level::Info, "gdt", "cpu index", cpu_index);
}

} // namespace tori::arch::x86_64
