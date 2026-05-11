#include <tori/kernel/memory_map.hpp>

namespace tori::memory {

const char* memory_kind_name(boot::MemoryKind kind) {
    switch (kind) {
    case boot::MemoryKind::Usable:
        return "usable";
    case boot::MemoryKind::Reserved:
        return "reserved";
    case boot::MemoryKind::AcpiReclaimable:
        return "acpi-reclaimable";
    case boot::MemoryKind::AcpiNvs:
        return "acpi-nvs";
    case boot::MemoryKind::BadMemory:
        return "bad-memory";
    case boot::MemoryKind::BootloaderReclaimable:
        return "bootloader-reclaimable";
    case boot::MemoryKind::KernelAndModules:
        return "kernel-and-modules";
    case boot::MemoryKind::Framebuffer:
        return "framebuffer";
    case boot::MemoryKind::ReservedMapped:
        return "reserved-mapped";
    case boot::MemoryKind::Unknown:
        return "unknown";
    }

    return "unknown";
}

bool is_initially_allocatable(boot::MemoryKind kind) {
    return kind == boot::MemoryKind::Usable || kind == boot::MemoryKind::BootloaderReclaimable;
}

MemorySummary summarize(const boot::MemoryMap& memory_map) {
    MemorySummary summary = {};

    for (size_t index = 0; index < memory_map.region_count; ++index) {
        const boot::MemoryRegion region = boot::memory_region_at(memory_map, index);

        if (region.length == 0 || region.base + region.length < region.base) {
            ++summary.invalid_regions;
            continue;
        }

        summary.total_bytes += region.length;

        switch (region.kind) {
        case boot::MemoryKind::Usable:
            summary.usable_bytes += region.length;
            break;
        case boot::MemoryKind::BootloaderReclaimable:
            summary.reclaimable_bytes += region.length;
            break;
        case boot::MemoryKind::Framebuffer:
            summary.framebuffer_bytes += region.length;
            break;
        default:
            summary.reserved_bytes += region.length;
            break;
        }
    }

    return summary;
}

} // namespace tori::memory
