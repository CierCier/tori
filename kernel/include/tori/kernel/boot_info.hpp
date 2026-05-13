#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tori::boot {

enum class BootSource : uint32_t {
    Unknown = 0,
    Limine = 1,
};

enum class MemoryKind : uint32_t {
    Usable,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    BadMemory,
    BootloaderReclaimable,
    KernelAndModules,
    Framebuffer,
    ReservedMapped,
    Unknown,
};

struct MemoryRegion {
    uint64_t base;
    uint64_t length;
    MemoryKind kind;
};

using MemoryRegionAt = MemoryRegion (*)(const void* context, size_t index);

struct MemoryMap {
    MemoryRegion* regions;
    size_t region_count;
    const void* source_context;
    MemoryRegionAt source_region_at;
};

inline MemoryRegion memory_region_at(const MemoryMap& memory_map, size_t index) {
    if (memory_map.regions != nullptr) {
        return memory_map.regions[index];
    }

    if (memory_map.source_region_at != nullptr) {
        return memory_map.source_region_at(memory_map.source_context, index);
    }

    return {};
}

enum class PixelFormat : uint32_t {
    Unknown,
    Rgb,
};

struct Framebuffer {
    void* address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bits_per_pixel;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    PixelFormat format;
};

struct KernelAddress {
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct CpuInfo {
    uint32_t processor_id;
    uint32_t lapic_id;
    void* internal_handle;
};

struct BootModule {
    uint64_t address;
    uint64_t size;
    char path[256];
};

struct SmpInfo {
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    CpuInfo* cpus;
    void (*wake_up_ap)(const CpuInfo& cpu, void (*entry)(void*), void* arg);
};

struct BootInfo {
    BootSource source;
    const char* bootloader_name;
    const char* bootloader_version;
    const char* command_line;
    KernelAddress kernel_address;
    uint64_t hhdm_offset;
    MemoryMap memory_map;
    Framebuffer framebuffer;
    void* rsdp;
    uint64_t module_count;
    BootModule* modules;
    SmpInfo smp;
    bool has_framebuffer;
    bool has_hhdm;
    bool has_rsdp;
    bool has_smp;
};

} // namespace tori::boot
