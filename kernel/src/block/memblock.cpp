#include <tori/kernel/block/memblock.hpp>
#include <tori/kernel/allocator.hpp>

namespace {

using namespace tori::block;

struct Memblock {
    const uint8_t* data;
    size_t size;
};

bool memblock_read(BlockDevice* dev, uint64_t sector, void* buf, size_t count) {
    auto* mb = static_cast<Memblock*>(dev->private_data);
    uint64_t offset = sector * dev->sector_size;
    uint64_t total = count * dev->sector_size;
    if (offset + total > mb->size || offset + total < offset) return false;
    auto* d = static_cast<uint8_t*>(buf);
    const auto* s = mb->data + offset;
    for (size_t i = 0; i < static_cast<size_t>(total); ++i) d[i] = s[i];
    return true;
}

bool memblock_write(BlockDevice*, uint64_t, const void*, size_t) {
    return false;
}

} // namespace

namespace tori::block {

BlockDevice* memblock_create(const void* data, size_t size) {
    auto* mb = static_cast<Memblock*>(tori::memory::kalloc(sizeof(Memblock), alignof(Memblock)));
    if (!mb) return nullptr;
    mb->data = static_cast<const uint8_t*>(data);
    mb->size = size;

    auto* dev = static_cast<BlockDevice*>(tori::memory::kalloc(sizeof(BlockDevice), alignof(BlockDevice)));
    if (!dev) {
        tori::memory::kfree(mb, sizeof(Memblock));
        return nullptr;
    }

    dev->sector_count = static_cast<uint64_t>(size / 512);
    dev->sector_size = 512;
    dev->read = memblock_read;
    dev->write = memblock_write;
    dev->private_data = mb;
    return dev;
}

} // namespace tori::block
