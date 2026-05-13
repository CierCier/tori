#pragma once

#include <stddef.h>
#include <tori/kernel/block_device.hpp>

namespace tori::block {

BlockDevice* memblock_create(const void* data, size_t size);

} // namespace tori::block
