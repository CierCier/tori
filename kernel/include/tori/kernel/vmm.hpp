#pragma once

#include <stdint.h>

namespace tori::memory::vmm {

enum class Flags : uint64_t {
    None          = 0,
    Present       = (1ULL << 0),
    Writable      = (1ULL << 1),
    User          = (1ULL << 2),
    WriteThrough  = (1ULL << 3),
    CacheDisable  = (1ULL << 4),
    Global        = (1ULL << 8),
    NoExecute     = (1ULL << 63),
};

inline Flags operator|(Flags a, Flags b) {
    return static_cast<Flags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline bool operator&(Flags a, Flags b) {
    return (static_cast<uint64_t>(a) & static_cast<uint64_t>(b)) != 0;
}

// Maps a single 4KiB page.
// If intermediate tables are missing, they will be allocated via PMM.
void map_page(uint64_t virtual_address, uint64_t physical_address, Flags flags);

} // namespace tori::memory::vmm
