#pragma once

#include <stdint.h>

#include <tori/kernel/boot_info.hpp>

namespace tori::memory {

struct MemorySummary {
    uint64_t total_bytes;
    uint64_t usable_bytes;
    uint64_t reclaimable_bytes;
    uint64_t reserved_bytes;
    uint64_t framebuffer_bytes;
    uint64_t invalid_regions;
};

MemorySummary summarize(const boot::MemoryMap& memory_map);
const char* memory_kind_name(boot::MemoryKind kind);
bool is_initially_allocatable(boot::MemoryKind kind);

} // namespace tori::memory
