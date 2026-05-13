#include <tori/kernel/fs/overlayfs.hpp>

#include <tori/kernel/allocator.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/string.hpp>

namespace {

using namespace tori::vfs;

constexpr const char* WHITEOUT_PREFIX = ".wh.";
constexpr size_t WHITEOUT_PREFIX_LEN = 4;

struct MergedEntry {
    char name[256];
    VnodeType type;
};

constexpr size_t MAX_LAYERS = 8;

struct OverlayVnode {
    Vnode* layer_roots[MAX_LAYERS];
    
    // Directory specific:
    MergedEntry* merged;
    size_t merged_count;
    bool cached;
};

tori::overlayfs::Layer g_layers[MAX_LAYERS];
size_t g_layer_count = 0;
bool g_initialized = false;

bool has_whiteout_prefix(const char* name) {
    for (size_t i = 0; i < WHITEOUT_PREFIX_LEN; ++i) {
        if (name[i] != WHITEOUT_PREFIX[i]) return false;
    }
    return true;
}

void make_whiteout_name(const char* name, char* out, size_t out_size) {
    size_t pos = 0;
    for (size_t i = 0; i < WHITEOUT_PREFIX_LEN && pos + 1 < out_size; ++i) {
        out[pos++] = WHITEOUT_PREFIX[i];
    }
    for (size_t i = 0; name[i] && pos + 1 < out_size; ++i) {
        out[pos++] = name[i];
    }
    out[pos] = '\0';
}

bool name_eq(const char* a, const char* b) {
    for (;;) {
        if (*a != *b) return false;
        if (*a == '\0') return true;
        ++a; ++b;
    }
}

OverlayVnode* alloc_overlay_vnode() {
    auto* ovn = static_cast<OverlayVnode*>(tori::memory::kalloc(sizeof(OverlayVnode), alignof(OverlayVnode)));
    if (!ovn) return nullptr;
    for (size_t i = 0; i < MAX_LAYERS; ++i) ovn->layer_roots[i] = nullptr;
    ovn->merged = nullptr;
    ovn->merged_count = 0;
    ovn->cached = false;
    return ovn;
}

Vnode* alloc_vnode(VnodeType type, void* private_data) {
    auto* vn = static_cast<Vnode*>(tori::memory::kalloc(sizeof(Vnode), alignof(Vnode)));
    if (!vn) return nullptr;
    vn->type = type;
    vn->private_data = private_data;
    vn->ref_count = 0;
    vn->mount_parent = nullptr;
    return vn;
}

void build_merged_list(OverlayVnode* ovn) {
    if (ovn->cached) return;

    size_t max_total = 0;
    for (size_t li = 0; li < g_layer_count; ++li) {
        auto* lroot = ovn->layer_roots[li];
        if (!lroot) continue;
        
        uint64_t offset = 0;
        Dirent d;
        while (lroot->ops->readdir(lroot, offset, &d, &offset) == 0) {
            max_total++;
        }
    }

    if (max_total == 0) {
        ovn->merged = nullptr;
        ovn->merged_count = 0;
        ovn->cached = true;
        return;
    }

    size_t alloc_size = max_total * sizeof(MergedEntry);
    auto* merged = static_cast<MergedEntry*>(tori::memory::kalloc(alloc_size, alignof(MergedEntry)));
    if (!merged) {
        ovn->merged = nullptr;
        ovn->merged_count = 0;
        ovn->cached = true;
        return;
    }

    size_t merged_count = 0;

    for (size_t li = 0; li < g_layer_count; ++li) {
        auto* lroot = ovn->layer_roots[li];
        if (!lroot) continue;
        
        uint64_t offset = 0;
        Dirent d;

        while (lroot->ops->readdir(lroot, offset, &d, &offset) == 0) {
            if (has_whiteout_prefix(d.name)) continue;

            // Check upper layers for whiteout
            bool whiteout = false;
            char wh_name[256];
            make_whiteout_name(d.name, wh_name, sizeof(wh_name));
            
            for (size_t u = 0; u < li; ++u) {
                if (!g_layers[u].writable || !ovn->layer_roots[u]) continue;
                
                Vnode* wh = nullptr;
                if (ovn->layer_roots[u]->ops->lookup(ovn->layer_roots[u], wh_name, &wh) == 0) {
                    whiteout = true;
                    vnode_unref(wh);
                    break;
                }
            }
            if (whiteout) continue;

            // Check if already in merged list (from an upper layer)
            bool already_seen = false;
            for (size_t m = 0; m < merged_count; ++m) {
                if (name_eq(merged[m].name, d.name)) {
                    already_seen = true;
                    break;
                }
            }
            if (already_seen) continue;

            tori::memory::copy_string(merged[merged_count].name, d.name, sizeof(merged[merged_count].name));
            merged[merged_count].type = d.type;
            ++merged_count;
        }
    }

    ovn->merged = merged;
    ovn->merged_count = merged_count;
    ovn->cached = true;
}

// Forward declarations of ops
int overlay_lookup(Vnode* dir, const char* name, Vnode** result);
int overlay_create(Vnode* dir, const char* name, Vnode** result);
int overlay_mkdir(Vnode* dir, const char* name);
int overlay_rmdir(Vnode* dir, const char* name);
int overlay_unlink(Vnode* dir, const char* name);
int overlay_read(Vnode* node, uint64_t offset, void* buf, size_t size, size_t* out_read);
int overlay_write(Vnode* node, uint64_t offset, const void* buf, size_t size, size_t* out_written);
int overlay_readdir(Vnode* dir, uint64_t offset, Dirent* entry, uint64_t* out_offset);
int overlay_stat(Vnode* node, Stat* stat);

VnodeOps overlay_vnode_ops = {
    .lookup  = overlay_lookup,
    .create  = overlay_create,
    .mkdir   = overlay_mkdir,
    .rmdir   = overlay_rmdir,
    .unlink  = overlay_unlink,
    .read    = overlay_read,
    .write   = overlay_write,
    .readdir = overlay_readdir,
    .stat    = overlay_stat,
};

int overlay_lookup(Vnode* dir, const char* name, Vnode** result) {
    auto* parent_ovn = static_cast<OverlayVnode*>(dir->private_data);
    
    auto* child_ovn = alloc_overlay_vnode();
    if (!child_ovn) return E_NO_SPACE;

    bool found = false;
    VnodeType type = VnodeType::File;

    char wh_name[256];
    make_whiteout_name(name, wh_name, sizeof(wh_name));

    for (size_t i = 0; i < g_layer_count; ++i) {
        if (!parent_ovn->layer_roots[i]) continue;
        
        auto* lroot = parent_ovn->layer_roots[i];

        // Check whiteout in writable layers above this one
        bool whiteout = false;
        for (size_t u = 0; u < i; ++u) {
            if (!g_layers[u].writable || !parent_ovn->layer_roots[u]) continue;
            
            Vnode* wh = nullptr;
            if (parent_ovn->layer_roots[u]->ops->lookup(parent_ovn->layer_roots[u], wh_name, &wh) == 0) {
                whiteout = true;
                vnode_unref(wh);
                break;
            }
        }
        if (whiteout) break; // Whiteout shadows everything below

        Vnode* real = nullptr;
        if (lroot->ops->lookup(lroot, name, &real) == 0) {
            if (!found) {
                type = real->type;
                found = true;
            }
            
            // If we already found a file, we stop here (files don't merge)
            // But if we found a directory, we continue to find other directory roots for merging
            if (type == VnodeType::File) {
                child_ovn->layer_roots[i] = real;
                break;
            } else {
                // If it's a directory, ensure subsequent layers also have directories
                if (real->type == VnodeType::Directory) {
                    child_ovn->layer_roots[i] = real;
                } else {
                    // Mismatched type? Shadowing file over directory or vice versa
                    // In a simple overlay, the topmost type wins.
                    vnode_unref(real);
                    break; 
                }
            }
        }
    }

    if (!found) {
        tori::memory::kfree(child_ovn, sizeof(OverlayVnode));
        return E_NOT_FOUND;
    }

    Vnode* vn = alloc_vnode(type, child_ovn);
    if (!vn) {
        // Should unref all collected layer_roots
        for (size_t i = 0; i < g_layer_count; ++i) {
            if (child_ovn->layer_roots[i]) vnode_unref(child_ovn->layer_roots[i]);
        }
        tori::memory::kfree(child_ovn, sizeof(OverlayVnode));
        return E_NO_SPACE;
    }
    vn->ops = &overlay_vnode_ops;
    *result = vn;
    return 0;
}

int overlay_create(Vnode* dir, const char* name, Vnode** result) {
    auto* ovn = static_cast<OverlayVnode*>(dir->private_data);

    for (size_t i = 0; i < g_layer_count; ++i) {
        if (!g_layers[i].writable || !ovn->layer_roots[i]) continue;

        Vnode* real = nullptr;
        int err = ovn->layer_roots[i]->ops->create(ovn->layer_roots[i], name, &real);
        if (err < 0) return err;

        auto* child_ovn = alloc_overlay_vnode();
        if (!child_ovn) {
            vnode_unref(real);
            return E_NO_SPACE;
        }
        child_ovn->layer_roots[i] = real;

        Vnode* vn = alloc_vnode(real->type, child_ovn);
        if (!vn) {
            vnode_unref(real);
            tori::memory::kfree(child_ovn, sizeof(OverlayVnode));
            return E_NO_SPACE;
        }
        vn->ops = &overlay_vnode_ops;

        *result = vn;
        return 0;
    }

    return E_IO;
}

int overlay_mkdir(Vnode* dir, const char* name) {
    auto* ovn = static_cast<OverlayVnode*>(dir->private_data);

    for (size_t i = 0; i < g_layer_count; ++i) {
        if (!g_layers[i].writable || !ovn->layer_roots[i]) continue;
        return ovn->layer_roots[i]->ops->mkdir(ovn->layer_roots[i], name);
    }

    return E_IO;
}

int overlay_rmdir(Vnode* dir, const char* name) {
    auto* ovn = static_cast<OverlayVnode*>(dir->private_data);

    for (size_t i = 0; i < g_layer_count; ++i) {
        if (!g_layers[i].writable || !ovn->layer_roots[i]) continue;

        Vnode* real = nullptr;
        if (ovn->layer_roots[i]->ops->lookup(ovn->layer_roots[i], name, &real) == 0) {
            vnode_unref(real);
            return ovn->layer_roots[i]->ops->rmdir(ovn->layer_roots[i], name);
        }
        break; // only check top writable layer
    }

    return E_NOT_FOUND;
}

int overlay_unlink(Vnode* dir, const char* name) {
    auto* ovn = static_cast<OverlayVnode*>(dir->private_data);

    for (size_t i = 0; i < g_layer_count; ++i) {
        if (!ovn->layer_roots[i]) continue;
        
        Vnode* real = nullptr;
        if (ovn->layer_roots[i]->ops->lookup(ovn->layer_roots[i], name, &real) != 0) continue;

        if (g_layers[i].writable) {
            vnode_unref(real);
            return ovn->layer_roots[i]->ops->unlink(ovn->layer_roots[i], name);
        }

        // File is on a read-only layer; create whiteout on top writable layer
        vnode_unref(real);
        for (size_t j = 0; j < i; ++j) {
            if (!g_layers[j].writable || !ovn->layer_roots[j]) continue;

            char wh_name[256];
            make_whiteout_name(name, wh_name, sizeof(wh_name));

            Vnode* wh = nullptr;
            if (ovn->layer_roots[j]->ops->lookup(ovn->layer_roots[j], wh_name, &wh) == 0) {
                vnode_unref(wh);
                return E_EXISTS;
            }

            Vnode* wh_vnode = nullptr;
            return ovn->layer_roots[j]->ops->create(ovn->layer_roots[j], wh_name, &wh_vnode);
        }

        return E_IO;
    }

    return E_NOT_FOUND;
}

int overlay_read(Vnode* node, uint64_t offset, void* buf, size_t size, size_t* out_read) {
    auto* ovn = static_cast<OverlayVnode*>(node->private_data);
    for (size_t i = 0; i < g_layer_count; ++i) {
        if (ovn->layer_roots[i]) {
            auto* real = ovn->layer_roots[i];
            if (!real->ops->read) return E_INVALID;
            return real->ops->read(real, offset, buf, size, out_read);
        }
    }
    return E_INVALID;
}

int overlay_write(Vnode* node, uint64_t offset, const void* buf, size_t size, size_t* out_written) {
    auto* ovn = static_cast<OverlayVnode*>(node->private_data);
    for (size_t i = 0; i < g_layer_count; ++i) {
        if (ovn->layer_roots[i]) {
            auto* real = ovn->layer_roots[i];
            if (!real->ops->write) return E_INVALID;
            return real->ops->write(real, offset, buf, size, out_written);
        }
    }
    return E_INVALID;
}

int overlay_readdir(Vnode* dir, uint64_t offset, Dirent* entry, uint64_t* out_offset) {
    auto* ovn = static_cast<OverlayVnode*>(dir->private_data);

    if (!ovn->cached) {
        build_merged_list(ovn);
    }

    if (offset >= ovn->merged_count) return E_NOT_FOUND;

    tori::memory::copy_string(entry->name, ovn->merged[offset].name, sizeof(entry->name));
    entry->type = ovn->merged[offset].type;
    *out_offset = offset + 1;
    return 0;
}

int overlay_stat(Vnode* node, Stat* stat) {
    auto* ovn = static_cast<OverlayVnode*>(node->private_data);
    for (size_t i = 0; i < g_layer_count; ++i) {
        if (ovn->layer_roots[i]) {
            auto* real = ovn->layer_roots[i];
            if (!real->ops->stat) return E_INVALID;
            return real->ops->stat(real, stat);
        }
    }
    return E_INVALID;
}

int overlay_mount(Vnode** out_root) {
    if (!g_initialized || g_layer_count == 0) return E_INVALID;

    auto* ovn = alloc_overlay_vnode();
    if (!ovn) return E_NO_SPACE;

    for (size_t i = 0; i < g_layer_count; ++i) {
        ovn->layer_roots[i] = g_layers[i].root;
        vnode_ref(ovn->layer_roots[i]);
    }

    Vnode* root = alloc_vnode(VnodeType::Directory, ovn);
    if (!root) {
        for (size_t i = 0; i < g_layer_count; ++i) vnode_unref(ovn->layer_roots[i]);
        tori::memory::kfree(ovn, sizeof(OverlayVnode));
        return E_NO_SPACE;
    }
    root->ops = &overlay_vnode_ops;

    *out_root = root;
    TORI_LOG_INFO("overlayfs", "mounted");
    return 0;
}

int overlay_unmount(Vnode* root) {
    auto* ovn = static_cast<OverlayVnode*>(root->private_data);
    if (ovn) {
        if (ovn->merged) {
            tori::memory::kfree(ovn->merged, ovn->merged_count * sizeof(MergedEntry));
        }
        for (size_t i = 0; i < g_layer_count; ++i) {
            if (ovn->layer_roots[i]) vnode_unref(ovn->layer_roots[i]);
        }
        tori::memory::kfree(ovn, sizeof(OverlayVnode));
    }
    tori::memory::kfree(root, sizeof(Vnode));
    return 0;
}

} // namespace

namespace tori::overlayfs {

int init(const Layer* layers, size_t count) {
    if (count == 0 || count > MAX_LAYERS) return -1;

    for (size_t i = 0; i < count; ++i) {
        g_layers[i] = layers[i];
    }
    g_layer_count = count;
    g_initialized = true;

    return 0;
}

void shutdown() {
    g_initialized = false;
    g_layer_count = 0;
}

FilesystemOps fs_ops = {
    .mount   = overlay_mount,
    .unmount = overlay_unmount,
};

} // namespace tori::overlayfs
