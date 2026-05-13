#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tori::block {

struct BlockDevice {
    uint64_t sector_count;
    uint16_t sector_size;
    bool (*read)(BlockDevice* dev, uint64_t sector, void* buf, size_t count);
    bool (*write)(BlockDevice* dev, uint64_t sector, const void* buf, size_t count);
    void* private_data;
};

} // namespace tori::block
