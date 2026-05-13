#include <tori/kernel/vfs.hpp>

#include <tori/kernel/log.hpp>

namespace {

using namespace tori::vfs;

struct MountEntry {
    Vnode* mount_point;
    Vnode* mounted_root;
    bool used;
};

MountEntry mount_table[CONFIG_VFS_MAX_MOUNTS];
tori::vfs::FileDescriptor fd_table[CONFIG_VFS_MAX_FDS];
Vnode* root_vnode = nullptr;
bool initialized = false;

int find_free_fd() {
    for (size_t i = 0; i < CONFIG_VFS_MAX_FDS; ++i) {
        if (!fd_table[i].used) return static_cast<int>(i);
    }
    return E_NO_SPACE;
}

int find_mount_by_root(Vnode* root) {
    for (size_t i = 0; i < CONFIG_VFS_MAX_MOUNTS; ++i) {
        if (mount_table[i].used && mount_table[i].mounted_root == root) {
            return static_cast<int>(i);
        }
    }
    return E_NOT_FOUND;
}

int find_free_mount_entry() {
    for (size_t i = 0; i < CONFIG_VFS_MAX_MOUNTS; ++i) {
        if (!mount_table[i].used) return static_cast<int>(i);
    }
    return E_NO_SPACE;
}

int find_mount_by_point(Vnode* point) {
    for (size_t i = 0; i < CONFIG_VFS_MAX_MOUNTS; ++i) {
        if (mount_table[i].used && mount_table[i].mount_point == point) {
            return static_cast<int>(i);
        }
    }
    return E_NOT_FOUND;
}

Vnode* resolve_mount(Vnode* vnode) {
    int idx = find_mount_by_point(vnode);
    if (idx >= 0) {
        return mount_table[idx].mounted_root;
    }
    return vnode;
}

int split_and_resolve(Vnode* base, const char* path, Vnode** result) {
    if (path == nullptr || path[0] == '\0') {
        *result = base;
        return E_SUCCESS;
    }

    Vnode* current = base;

    const char* p = path;
    while (*p != '\0') {
        while (*p == '/') ++p;
        if (*p == '\0') break;

        const char* start = p;
        while (*p != '\0' && *p != '/') ++p;

        size_t component_len = static_cast<size_t>(p - start);
        if (component_len > 255) return E_INVALID;

        if (current->type != VnodeType::Directory) return E_NOT_DIR;

        char component[256];
        for (size_t i = 0; i < component_len; ++i) {
            component[i] = start[i];
        }
        component[component_len] = '\0';

        Vnode* next = nullptr;
        int err = current->ops->lookup(current, component, &next);
        if (err < 0) return err;

        current = resolve_mount(next);
    }

    *result = current;
    return E_SUCCESS;
}

} // namespace

namespace tori::vfs {

void init() {
    for (size_t i = 0; i < CONFIG_VFS_MAX_MOUNTS; ++i) {
        mount_table[i].used = false;
    }
    for (size_t i = 0; i < CONFIG_VFS_MAX_FDS; ++i) {
        fd_table[i].used = false;
    }
    root_vnode = nullptr;
    initialized = true;
    TORI_LOG_INFO("vfs", "VFS initialized");
}

int mount(FilesystemOps* fs_ops, Vnode* target, Vnode** out_root) {
    if (!initialized || !fs_ops) return E_INVALID;

    Vnode* new_root = nullptr;
    int err = fs_ops->mount(&new_root);
    if (err < 0) return err;
    if (!new_root) return E_IO;

    if (target == nullptr) {
        if (root_vnode != nullptr) {
            fs_ops->unmount(new_root);
            return E_EXISTS;
        }
        root_vnode = new_root;
        if (out_root) *out_root = new_root;
        TORI_LOG_INFO("vfs", "root filesystem mounted");
        return E_SUCCESS;
    }

    int idx = find_free_mount_entry();
    if (idx < 0) {
        fs_ops->unmount(new_root);
        return E_NO_SPACE;
    }

    new_root->mount_parent = target;
    mount_table[idx].mount_point = target;
    mount_table[idx].mounted_root = new_root;
    mount_table[idx].used = true;

    if (out_root) *out_root = new_root;
    TORI_LOG_INFO("vfs", "filesystem mounted");
    return E_SUCCESS;
}

int unmount(Vnode* mount_root) {
    if (!initialized || !mount_root) return E_INVALID;

    int idx = find_mount_by_root(mount_root);
    if (idx < 0) return E_NOT_FOUND;

    mount_table[idx].used = false;
    if (mount_root->ops && mount_root->ops->stat) {
        // We'd call unmount ops here if we had a way to get back to fs_ops
        // For now, just remove from table and let the FS handle it
    }

    if (mount_root == root_vnode) {
        root_vnode = nullptr;
    }

    TORI_LOG_INFO("vfs", "filesystem unmounted");
    return E_SUCCESS;
}

int set_root(Vnode* new_root) {
    if (!initialized || !new_root) return E_INVALID;
    root_vnode = new_root;
    return E_SUCCESS;
}

int resolve(Vnode* base, const char* path, Vnode** result) {
    if (!initialized) return E_INVALID;
    if (!result) return E_INVALID;

    if (path == nullptr || path[0] == '\0') {
        *result = base;
        return E_SUCCESS;
    }

    Vnode* start = base;
    if (path[0] == '/') {
        if (root_vnode == nullptr) return E_NOT_FOUND;
        start = root_vnode;
        while (*path == '/') ++path;
        if (*path == '\0') {
            *result = root_vnode;
            return E_SUCCESS;
        }
    }

    return split_and_resolve(start, path, result);
}

int open(Vnode* base, const char* path, uint32_t flags, int* out_fd) {
    if (!initialized) return E_INVALID;
    if (!path || !out_fd) return E_INVALID;

    Vnode* vnode = nullptr;
    int err = resolve(base, path, &vnode);
    if (err < 0) {
        if (err == E_NOT_FOUND && (flags & O_CREAT)) {
            const char* sep = nullptr;
            for (const char* s = path; *s; ++s) {
                if (*s == '/') sep = s;
            }

            Vnode* parent = nullptr;
            if (sep == nullptr) {
                parent = base;
            } else {
                char parent_path[256];
                size_t plen = static_cast<size_t>(sep - path);
                for (size_t i = 0; i < plen; ++i) parent_path[i] = path[i];
                parent_path[plen] = '\0';
                err = resolve(base, parent_path, &parent);
                if (err < 0) return err;
            }

            const char* name = (sep != nullptr) ? sep + 1 : path;
            err = parent->ops->create(parent, name, &vnode);
            if (err < 0) return err;
        } else {
            return err;
        }
    }

    int fd = find_free_fd();
    if (fd < 0) return E_NO_SPACE;

    vnode_ref(vnode);
    fd_table[fd].vnode = vnode;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = flags;
    fd_table[fd].used = true;

    *out_fd = fd;
    return E_SUCCESS;
}

int close(int fd) {
    if (fd < 0 || fd >= CONFIG_VFS_MAX_FDS) return E_BAD_FD;
    if (!fd_table[fd].used) return E_BAD_FD;

    vnode_unref(fd_table[fd].vnode);
    fd_table[fd].used = false;
    return E_SUCCESS;
}

int read(int fd, void* buf, size_t size, size_t* out_read) {
    if (fd < 0 || fd >= CONFIG_VFS_MAX_FDS) return E_BAD_FD;
    if (!fd_table[fd].used) return E_BAD_FD;

    auto* f = &fd_table[fd];
    if (f->vnode->type != VnodeType::File) return E_IS_DIR;
    if (!f->vnode->ops->read) return E_INVALID;

    size_t nread = 0;
    int err = f->vnode->ops->read(f->vnode, f->offset, buf, size, &nread);
    if (err == 0) {
        f->offset += nread;
        if (out_read) *out_read = nread;
    }
    return err;
}

int write(int fd, const void* buf, size_t size, size_t* out_written) {
    if (fd < 0 || fd >= CONFIG_VFS_MAX_FDS) return E_BAD_FD;
    if (!fd_table[fd].used) return E_BAD_FD;

    auto* f = &fd_table[fd];
    if (f->vnode->type != VnodeType::File) return E_IS_DIR;
    if (!f->vnode->ops->write) return E_INVALID;

    size_t nwritten = 0;
    int err = f->vnode->ops->write(f->vnode, f->offset, buf, size, &nwritten);
    if (err == 0) {
        f->offset += nwritten;
        if (out_written) *out_written = nwritten;
    }
    return err;
}

int readdir(int fd, Dirent* entry) {
    if (fd < 0 || fd >= CONFIG_VFS_MAX_FDS) return E_BAD_FD;
    if (!fd_table[fd].used) return E_BAD_FD;

    auto* f = &fd_table[fd];
    if (f->vnode->type != VnodeType::Directory) return E_NOT_DIR;
    if (!f->vnode->ops->readdir) return E_INVALID;

    return f->vnode->ops->readdir(f->vnode, f->offset, entry, &f->offset);
}

int stat(int fd, Stat* stat) {
    if (fd < 0 || fd >= CONFIG_VFS_MAX_FDS) return E_BAD_FD;
    if (!fd_table[fd].used) return E_BAD_FD;

    auto* f = &fd_table[fd];
    if (!f->vnode->ops->stat) return E_INVALID;

    return f->vnode->ops->stat(f->vnode, stat);
}

int seek(int fd, int64_t offset, int whence, uint64_t* out_pos) {
    if (fd < 0 || fd >= CONFIG_VFS_MAX_FDS) return E_BAD_FD;
    if (!fd_table[fd].used) return E_BAD_FD;

    auto* f = &fd_table[fd];
    uint64_t new_offset;

    if (whence == 0) {
        new_offset = static_cast<uint64_t>(offset);
    } else if (whence == 1) {
        new_offset = f->offset + static_cast<uint64_t>(offset);
    } else {
        Stat s;
        int err = f->vnode->ops->stat(f->vnode, &s);
        if (err < 0) return err;
        new_offset = s.size + static_cast<uint64_t>(offset);
    }

    f->offset = new_offset;
    if (out_pos) *out_pos = new_offset;
    return E_SUCCESS;
}

void vnode_ref(Vnode* vnode) {
    if (vnode) {
        ++vnode->ref_count;
    }
}

void vnode_unref(Vnode* vnode) {
    if (vnode && vnode->ref_count > 0) {
        --vnode->ref_count;
    }
}

} // namespace tori::vfs
