#pragma once

#include <tori/kernel/block_device.hpp>
#include <tori/kernel/vfs.hpp>

namespace tori::fat {

int init(tori::block::BlockDevice* dev);
void shutdown();

extern vfs::FilesystemOps fs_ops;

} // namespace tori::fat
