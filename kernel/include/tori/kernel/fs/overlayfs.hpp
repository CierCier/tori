#pragma once

#include <tori/kernel/vfs.hpp>
#include <config.h>

namespace tori::overlayfs {

struct Layer {
    vfs::Vnode* root;
    bool writable;
};

int init(const Layer* layers, size_t count);
void shutdown();

extern vfs::FilesystemOps fs_ops;

} // namespace tori::overlayfs
