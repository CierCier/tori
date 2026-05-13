#include <tori/kernel/fs/ramfs.hpp>

#include <tori/kernel/allocator.hpp>
#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/string.hpp>

namespace {

using namespace tori::vfs;

constexpr size_t kPageSize = 4096;

struct RamfsEntry {
    char name[256];
    Vnode* vnode;
};

struct RamfsDir {
    RamfsEntry* entries;
    size_t count;
    size_t capacity;
};

struct RamfsFile {
    uintptr_t* pages;
    size_t page_count;
    size_t size;
};

RamfsDir* alloc_dir() {
    auto* dir = static_cast<RamfsDir*>(tori::memory::kalloc(sizeof(RamfsDir), alignof(RamfsDir)));
    if (!dir) return nullptr;
    dir->entries = nullptr;
    dir->count = 0;
    dir->capacity = 0;
    return dir;
}

RamfsFile* alloc_file() {
    auto* file = static_cast<RamfsFile*>(tori::memory::kalloc(sizeof(RamfsFile), alignof(RamfsFile)));
    if (!file) return nullptr;
    file->pages = nullptr;
    file->page_count = 0;
    file->size = 0;
    return file;
}

Vnode* create_vnode(VnodeType type) {
    auto* vnode = static_cast<Vnode*>(tori::memory::kalloc(sizeof(Vnode), alignof(Vnode)));
    if (!vnode) return nullptr;
    vnode->type = type;
    vnode->private_data = nullptr;
    vnode->ref_count = 1;
    vnode->mount_parent = nullptr;
    return vnode;
}

int ensure_dir_capacity(RamfsDir* dir, size_t needed) {
    if (needed <= dir->capacity) return 0;

    size_t new_cap = dir->capacity ? dir->capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;

    auto* new_entries = static_cast<RamfsEntry*>(
        tori::memory::kalloc(new_cap * sizeof(RamfsEntry), alignof(RamfsEntry)));
    if (!new_entries) return E_NO_SPACE;

    for (size_t i = 0; i < dir->count; ++i) {
        new_entries[i] = dir->entries[i];
    }

    if (dir->entries) {
        tori::memory::kfree(dir->entries, dir->capacity * sizeof(RamfsEntry));
    }

    dir->entries = new_entries;
    dir->capacity = new_cap;
    return 0;
}

int dir_add_entry(RamfsDir* dir, const char* name, Vnode* vnode) {
    int err = ensure_dir_capacity(dir, dir->count + 1);
    if (err < 0) return err;

    RamfsEntry* e = &dir->entries[dir->count];
    tori::memory::copy_string(e->name, name, sizeof(e->name));
    e->vnode = vnode;
    vnode_ref(vnode);
    dir->count++;
    return 0;
}

int dir_find_entry(RamfsDir* dir, const char* name, size_t* out_idx) {
    for (size_t i = 0; i < dir->count; ++i) {
        if (tori::memory::string_equals(dir->entries[i].name, name)) {
            *out_idx = i;
            return 0;
        }
    }
    return E_NOT_FOUND;
}

void dir_remove_entry(RamfsDir* dir, size_t idx) {
    if (idx >= dir->count) return;
    vnode_unref(dir->entries[idx].vnode);
    for (size_t i = idx + 1; i < dir->count; ++i) {
        dir->entries[i - 1] = dir->entries[i];
    }
    dir->count--;
}

// VnodeOps implementations

int ramfs_lookup(Vnode* dir, const char* name, Vnode** result) {
    auto* ramdir = static_cast<RamfsDir*>(dir->private_data);
    size_t idx;
    if (dir_find_entry(ramdir, name, &idx) < 0) return E_NOT_FOUND;
    *result = ramdir->entries[idx].vnode;
    return 0;
}

int ramfs_create(Vnode* dir, const char* name, Vnode** result) {
    auto* ramdir = static_cast<RamfsDir*>(dir->private_data);

    size_t idx;
    if (dir_find_entry(ramdir, name, &idx) == 0) return E_EXISTS;

    Vnode* vnode = create_vnode(VnodeType::File);
    if (!vnode) return E_NO_SPACE;

    vnode->ops = dir->ops; // same ops table
    RamfsFile* file = alloc_file();
    if (!file) {
        tori::memory::kfree(vnode, sizeof(Vnode));
        return E_NO_SPACE;
    }
    vnode->private_data = file;

    int err = dir_add_entry(ramdir, name, vnode);
    if (err < 0) {
        tori::memory::kfree(file, sizeof(RamfsFile));
        tori::memory::kfree(vnode, sizeof(Vnode));
        return err;
    }

    *result = vnode;
    return 0;
}

int ramfs_mkdir(Vnode* dir, const char* name) {
    auto* ramdir = static_cast<RamfsDir*>(dir->private_data);

    size_t idx;
    if (dir_find_entry(ramdir, name, &idx) == 0) return E_EXISTS;

    Vnode* vnode = create_vnode(VnodeType::Directory);
    if (!vnode) return E_NO_SPACE;

    vnode->ops = dir->ops;
    RamfsDir* subdir = alloc_dir();
    if (!subdir) {
        tori::memory::kfree(vnode, sizeof(Vnode));
        return E_NO_SPACE;
    }
    vnode->private_data = subdir;

    int err = dir_add_entry(ramdir, name, vnode);
    if (err < 0) {
        tori::memory::kfree(subdir, sizeof(RamfsDir));
        tori::memory::kfree(vnode, sizeof(Vnode));
        return err;
    }

    return 0;
}

int ramfs_rmdir(Vnode* dir, const char* name) {
    auto* ramdir = static_cast<RamfsDir*>(dir->private_data);
    size_t idx;
    if (dir_find_entry(ramdir, name, &idx) < 0) return E_NOT_FOUND;

    Vnode* target = ramdir->entries[idx].vnode;
    if (target->type != VnodeType::Directory) return E_NOT_DIR;

    auto* target_dir = static_cast<RamfsDir*>(target->private_data);
    if (target_dir->count > 0) return E_NOT_EMPTY;

    tori::memory::kfree(target_dir, sizeof(RamfsDir));
    tori::memory::kfree(target, sizeof(Vnode));
    dir_remove_entry(ramdir, idx);
    return 0;
}

int ramfs_unlink(Vnode* dir, const char* name) {
    auto* ramdir = static_cast<RamfsDir*>(dir->private_data);
    size_t idx;
    if (dir_find_entry(ramdir, name, &idx) < 0) return E_NOT_FOUND;

    Vnode* target = ramdir->entries[idx].vnode;
    if (target->type != VnodeType::File) return E_IS_DIR;

    auto* file = static_cast<RamfsFile*>(target->private_data);
    for (size_t i = 0; i < file->page_count; ++i) {
        if (file->pages[i]) {
            tori::memory::pmm::free_page(file->pages[i]);
        }
    }
    if (file->pages) {
        tori::memory::kfree(file->pages, file->page_count * sizeof(uintptr_t));
    }
    tori::memory::kfree(file, sizeof(RamfsFile));
    tori::memory::kfree(target, sizeof(Vnode));
    dir_remove_entry(ramdir, idx);
    return 0;
}

int ramfs_read(Vnode* node, uint64_t offset, void* buf, size_t size, size_t* out_read) {
    auto* file = static_cast<RamfsFile*>(node->private_data);
    if (offset >= file->size) {
        *out_read = 0;
        return 0;
    }

    size_t available = file->size - static_cast<size_t>(offset);
    if (size > available) size = available;

    size_t done = 0;
    while (done < size) {
        size_t page_idx = static_cast<size_t>(offset + done) / kPageSize;
        size_t page_off = static_cast<size_t>(offset + done) % kPageSize;
        size_t chunk = kPageSize - page_off;
        if (chunk > size - done) chunk = size - done;

        if (page_idx < file->page_count && file->pages[page_idx]) {
            auto* src = reinterpret_cast<const uint8_t*>(
                tori::memory::address::physical_to_virtual(file->pages[page_idx]));
            auto* dst = static_cast<uint8_t*>(buf) + done;
            for (size_t i = 0; i < chunk; ++i) dst[i] = src[page_off + i];
        }

        done += chunk;
    }

    *out_read = done;
    return 0;
}

int ramfs_write(Vnode* node, uint64_t offset, const void* buf, size_t size, size_t* out_written) {
    auto* file = static_cast<RamfsFile*>(node->private_data);

    uint64_t end = offset + size;
    size_t needed_pages = (end + kPageSize - 1) / kPageSize;

    if (needed_pages > file->page_count) {
        auto* new_pages = static_cast<uintptr_t*>(
            tori::memory::kalloc(needed_pages * sizeof(uintptr_t), alignof(uintptr_t)));
        if (!new_pages) return E_NO_SPACE;

        for (size_t i = 0; i < file->page_count; ++i) new_pages[i] = file->pages[i];
        for (size_t i = file->page_count; i < needed_pages; ++i) new_pages[i] = 0;

        if (file->pages) {
            tori::memory::kfree(file->pages, file->page_count * sizeof(uintptr_t));
        }
        file->pages = new_pages;
        file->page_count = needed_pages;
    }

    size_t done = 0;
    while (done < size) {
        size_t page_idx = static_cast<size_t>(offset + done) / kPageSize;
        size_t page_off = static_cast<size_t>(offset + done) % kPageSize;
        size_t chunk = kPageSize - page_off;
        if (chunk > size - done) chunk = size - done;

        if (file->pages[page_idx] == 0) {
            uint64_t phys = tori::memory::pmm::alloc_page();
            if (phys == tori::memory::pmm::invalid_physical_address) return E_NO_SPACE;
            file->pages[page_idx] = phys;
        }

        auto* dst = reinterpret_cast<uint8_t*>(
            tori::memory::address::physical_to_virtual(file->pages[page_idx]));
        auto* src = static_cast<const uint8_t*>(buf) + done;
        for (size_t i = 0; i < chunk; ++i) dst[page_off + i] = src[i];

        done += chunk;
    }

    if (end > file->size) file->size = static_cast<size_t>(end);
    *out_written = size;
    return 0;
}

int ramfs_readdir(Vnode* dir, uint64_t offset, Dirent* entry, uint64_t* out_offset) {
    auto* ramdir = static_cast<RamfsDir*>(dir->private_data);
    if (offset >= ramdir->count) return E_NOT_FOUND;

    auto& e = ramdir->entries[offset];
    tori::memory::copy_string(entry->name, e.name, sizeof(entry->name));
    entry->type = e.vnode->type;
    *out_offset = offset + 1;
    return 0;
}

int ramfs_stat(Vnode* node, Stat* stat) {
    stat->type = node->type;
    if (node->type == VnodeType::File) {
        auto* file = static_cast<RamfsFile*>(node->private_data);
        stat->size = file->size;
    } else {
        stat->size = 0;
    }
    return 0;
}

VnodeOps ramfs_vnode_ops = {
    .lookup  = ramfs_lookup,
    .create  = ramfs_create,
    .mkdir   = ramfs_mkdir,
    .rmdir   = ramfs_rmdir,
    .unlink  = ramfs_unlink,
    .read    = ramfs_read,
    .write   = ramfs_write,
    .readdir = ramfs_readdir,
    .stat    = ramfs_stat,
};

int ramfs_mount(Vnode** out_root) {
    Vnode* root = create_vnode(VnodeType::Directory);
    if (!root) return E_NO_SPACE;

    RamfsDir* dir = alloc_dir();
    if (!dir) {
        tori::memory::kfree(root, sizeof(Vnode));
        return E_NO_SPACE;
    }

    root->ops = &ramfs_vnode_ops;
    root->private_data = dir;
    *out_root = root;

    TORI_LOG_INFO("ramfs", "mounted");
    return 0;
}

int ramfs_unmount(Vnode* root) {
    if (root->type == VnodeType::Directory) {
        auto* dir = static_cast<RamfsDir*>(root->private_data);
        if (dir->entries) {
            tori::memory::kfree(dir->entries, dir->capacity * sizeof(RamfsEntry));
        }
        tori::memory::kfree(dir, sizeof(RamfsDir));
    }
    tori::memory::kfree(root, sizeof(Vnode));
    TORI_LOG_INFO("ramfs", "unmounted");
    return 0;
}

} // namespace

namespace tori::ramfs {

vfs::FilesystemOps fs_ops = {
    .mount   = ramfs_mount,
    .unmount = ramfs_unmount,
};

} // namespace tori::ramfs
