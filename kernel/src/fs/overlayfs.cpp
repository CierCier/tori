#include <tori/kernel/fs/overlayfs.hpp>

#include <tori/kernel/allocator.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/string.hpp>

namespace {

using namespace tori::vfs;

constexpr const char* WHITEOUT_PREFIX = ".wh.";
constexpr size_t WHITEOUT_PREFIX_LEN = 4;

struct OverlayNode {
    Vnode* real_node;
    Vnode* overlay_parent;
};

struct MergedEntry {
    char name[256];
    VnodeType type;
};

struct OverlayDir {
    tori::overlayfs::Layer* layers;
    size_t layer_count;
    MergedEntry* merged;
    size_t merged_count;
    bool cached;
};

constexpr size_t MAX_LAYERS = 8;
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

Vnode* alloc_vnode(VnodeType type, void* private_data) {
    auto* vn = static_cast<Vnode*>(tori::memory::kalloc(sizeof(Vnode), alignof(Vnode)));
    if (!vn) return nullptr;
    vn->type = type;
    vn->private_data = private_data;
    vn->ref_count = 0;
    vn->mount_parent = nullptr;
    return vn;
}

void build_merged_list(OverlayDir* odir) {
    if (odir->cached) return;

    size_t max_total = 0;
    size_t layer_entry_counts[MAX_LAYERS];
    for (size_t li = 0; li < odir->layer_count; ++li) {
        auto* lroot = odir->layers[li].root;
        uint64_t offset = 0;
        Dirent d;
        size_t count = 0;
        while (lroot->ops->readdir(lroot, offset, &d, &offset) == 0) {
            ++count;
        }
        layer_entry_counts[li] = count;
        max_total += count;
    }

    if (max_total == 0) {
        odir->merged = nullptr;
        odir->merged_count = 0;
        odir->cached = true;
        return;
    }

    size_t alloc_size = max_total * sizeof(MergedEntry);
    auto* merged = static_cast<MergedEntry*>(tori::memory::kalloc(alloc_size, alignof(MergedEntry)));
    if (!merged) {
        odir->merged = nullptr;
        odir->merged_count = 0;
        odir->cached = true;
        return;
    }

    size_t merged_count = 0;

    for (size_t li = 0; li < odir->layer_count; ++li) {
        auto* lroot = odir->layers[li].root;
        uint64_t offset = 0;
        Dirent d;

        while (lroot->ops->readdir(lroot, offset, &d, &offset) == 0) {
            if (has_whiteout_prefix(d.name)) {
                continue;
            }

            // Check upper layers for whiteout
            bool whiteout = false;
            for (size_t u = 0; u < li; ++u) {
                if (!odir->layers[u].writable) continue;
                char wh_name[256];
                make_whiteout_name(d.name, wh_name, sizeof(wh_name));
                Vnode* wh = nullptr;
                if (odir->layers[u].root->ops->lookup(odir->layers[u].root, wh_name, &wh) == 0) {
                    whiteout = true;
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

    odir->merged = merged;
    odir->merged_count = merged_count;
    odir->cached = true;
}

int overlay_lookup(Vnode* dir, const char* name, Vnode** result) {
    auto* odir = static_cast<OverlayDir*>(dir->private_data);

    for (size_t i = 0; i < odir->layer_count; ++i) {
        auto* lroot = odir->layers[i].root;

        // Check whiteout in writable layers above this one
        char wh_name[256];
        make_whiteout_name(name, wh_name, sizeof(wh_name));
        for (size_t u = 0; u <= i; ++u) {
            if (!odir->layers[u].writable) continue;
            // Check upper layer for whiteout of this name
            Vnode* wh = nullptr;
            if (u < i) {
                if (odir->layers[u].root->ops->lookup(odir->layers[u].root, wh_name, &wh) == 0) {
                    return E_NOT_FOUND;
                }
            }
        }

        // Check whiteout on current layer
        if (i == 0) {
            Vnode* wh = nullptr;
            if (odir->layers[i].writable && odir->layers[i].root->ops->lookup(odir->layers[i].root, wh_name, &wh) == 0) {
                return E_NOT_FOUND;
            }
        }

        Vnode* real = nullptr;
        int err = lroot->ops->lookup(lroot, name, &real);
        if (err == 0) {
            auto* on = static_cast<OverlayNode*>(tori::memory::kalloc(sizeof(OverlayNode), alignof(OverlayNode)));
            if (!on) return E_NO_SPACE;
            on->real_node = real;
            on->overlay_parent = dir;

            Vnode* vn = alloc_vnode(real->type, on);
            if (!vn) {
                tori::memory::kfree(on, sizeof(OverlayNode));
                return E_NO_SPACE;
            }
            vn->ops = dir->ops;

            *result = vn;
            return 0;
        }
    }

    return E_NOT_FOUND;
}

int overlay_create(Vnode* dir, const char* name, Vnode** result) {
    auto* odir = static_cast<OverlayDir*>(dir->private_data);

    for (size_t i = 0; i < odir->layer_count; ++i) {
        if (!odir->layers[i].writable) continue;

        Vnode* real = nullptr;
        int err = odir->layers[i].root->ops->create(odir->layers[i].root, name, &real);
        if (err < 0) return err;

        auto* on = static_cast<OverlayNode*>(tori::memory::kalloc(sizeof(OverlayNode), alignof(OverlayNode)));
        if (!on) return E_NO_SPACE;
        on->real_node = real;
        on->overlay_parent = dir;

        Vnode* vn = alloc_vnode(real->type, on);
        if (!vn) {
            tori::memory::kfree(on, sizeof(OverlayNode));
            return E_NO_SPACE;
        }
        vn->ops = dir->ops;

        *result = vn;
        return 0;
    }

    return E_IO;
}

int overlay_mkdir(Vnode* dir, const char* name) {
    auto* odir = static_cast<OverlayDir*>(dir->private_data);

    for (size_t i = 0; i < odir->layer_count; ++i) {
        if (!odir->layers[i].writable) continue;
        return odir->layers[i].root->ops->mkdir(odir->layers[i].root, name);
    }

    return E_IO;
}

int overlay_rmdir(Vnode* dir, const char* name) {
    auto* odir = static_cast<OverlayDir*>(dir->private_data);

    for (size_t i = 0; i < odir->layer_count; ++i) {
        if (!odir->layers[i].writable) continue;

        // Only rmdir from writable layer if it exists there
        Vnode* real = nullptr;
        if (odir->layers[i].root->ops->lookup(odir->layers[i].root, name, &real) == 0) {
            return odir->layers[i].root->ops->rmdir(odir->layers[i].root, name);
        }
        break; // only check top writable layer
    }

    return E_NOT_FOUND;
}

int overlay_unlink(Vnode* dir, const char* name) {
    auto* odir = static_cast<OverlayDir*>(dir->private_data);

    for (size_t i = 0; i < odir->layer_count; ++i) {
        Vnode* real = nullptr;
        if (odir->layers[i].root->ops->lookup(odir->layers[i].root, name, &real) != 0) continue;

        if (odir->layers[i].writable) {
            return odir->layers[i].root->ops->unlink(odir->layers[i].root, name);
        }

        // File is on a read-only layer; create whiteout on top writable layer
        for (size_t j = 0; j < i; ++j) {
            if (!odir->layers[j].writable) continue;

            char wh_name[256];
            make_whiteout_name(name, wh_name, sizeof(wh_name));

            // Check if whiteout already exists
            Vnode* wh = nullptr;
            if (odir->layers[j].root->ops->lookup(odir->layers[j].root, wh_name, &wh) == 0) {
                return E_EXISTS;
            }

            Vnode* wh_vnode = nullptr;
            return odir->layers[j].root->ops->create(odir->layers[j].root, wh_name, &wh_vnode);
        }

        return E_IO;
    }

    return E_NOT_FOUND;
}

int overlay_read(Vnode* node, uint64_t offset, void* buf, size_t size, size_t* out_read) {
    auto* on = static_cast<OverlayNode*>(node->private_data);
    if (!on->real_node->ops->read) return E_INVALID;
    return on->real_node->ops->read(on->real_node, offset, buf, size, out_read);
}

int overlay_write(Vnode* node, uint64_t offset, const void* buf, size_t size, size_t* out_written) {
    auto* on = static_cast<OverlayNode*>(node->private_data);
    if (!on->real_node->ops->write) return E_INVALID;
    return on->real_node->ops->write(on->real_node, offset, buf, size, out_written);
}

int overlay_readdir(Vnode* dir, uint64_t offset, Dirent* entry, uint64_t* out_offset) {
    auto* odir = static_cast<OverlayDir*>(dir->private_data);

    if (!odir->cached) {
        build_merged_list(odir);
    }

    if (offset >= odir->merged_count) return E_NOT_FOUND;

    tori::memory::copy_string(entry->name, odir->merged[offset].name, sizeof(entry->name));
    entry->type = odir->merged[offset].type;
    *out_offset = offset + 1;
    return 0;
}

int overlay_stat(Vnode* node, Stat* stat) {
    auto* on = static_cast<OverlayNode*>(node->private_data);
    if (!on->real_node->ops->stat) return E_INVALID;
    return on->real_node->ops->stat(on->real_node, stat);
}

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

int overlay_mount(Vnode** out_root) {
    if (!g_initialized || g_layer_count == 0) return E_INVALID;

    auto* odir = static_cast<OverlayDir*>(tori::memory::kalloc(sizeof(OverlayDir), alignof(OverlayDir)));
    if (!odir) return E_NO_SPACE;

    odir->layers = g_layers;
    odir->layer_count = g_layer_count;
    odir->merged = nullptr;
    odir->merged_count = 0;
    odir->cached = false;

    Vnode* root = alloc_vnode(VnodeType::Directory, odir);
    if (!root) {
        tori::memory::kfree(odir, sizeof(OverlayDir));
        return E_NO_SPACE;
    }
    root->ops = &overlay_vnode_ops;

    *out_root = root;
    TORI_LOG_INFO("overlayfs", "mounted");
    return 0;
}

int overlay_unmount(Vnode* root) {
    bool found = false;
    for (size_t i = 0; i < g_layer_count && !found; ++i) {
        if (g_layers[i].root == root) found = true;
    }
    if (!found) {
        // Free overlay dir data
        auto* odir = static_cast<OverlayDir*>(root->private_data);
        if (odir) {
            if (odir->merged) {
                tori::memory::kfree(odir->merged, odir->merged_count * sizeof(MergedEntry));
            }
            tori::memory::kfree(odir, sizeof(OverlayDir));
        }
        tori::memory::kfree(root, sizeof(Vnode));
    }
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
