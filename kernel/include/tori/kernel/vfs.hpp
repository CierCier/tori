#pragma once

#include <stdint.h>
#include <stddef.h>

#include <config.h>

namespace tori::vfs {

constexpr int E_SUCCESS   = 0;
constexpr int E_NOT_FOUND = -1;
constexpr int E_EXISTS    = -2;
constexpr int E_NOT_DIR   = -3;
constexpr int E_IS_DIR    = -4;
constexpr int E_INVALID   = -5;
constexpr int E_NO_SPACE  = -6;
constexpr int E_IO        = -7;
constexpr int E_BAD_FD    = -8;
constexpr int E_NOT_EMPTY = -9;

constexpr uint32_t O_RDONLY = 0;
constexpr uint32_t O_WRONLY = 1;
constexpr uint32_t O_RDWR   = 2;
constexpr uint32_t O_CREAT  = 4;

enum class VnodeType : uint8_t { File, Directory };

struct Stat {
    VnodeType type;
    uint64_t size;
};

struct Dirent {
    char name[256];
    VnodeType type;
};

struct Vnode;

struct VnodeOps {
    int (*lookup)(Vnode* dir, const char* name, Vnode** result);
    int (*create)(Vnode* dir, const char* name, Vnode** result);
    int (*mkdir)(Vnode* dir, const char* name);
    int (*rmdir)(Vnode* dir, const char* name);
    int (*unlink)(Vnode* dir, const char* name);
    int (*read)(Vnode* node, uint64_t offset, void* buf, size_t size, size_t* out_read);
    int (*write)(Vnode* node, uint64_t offset, const void* buf, size_t size, size_t* out_written);
    int (*readdir)(Vnode* dir, uint64_t offset, Dirent* entry, uint64_t* out_offset);
    int (*stat)(Vnode* node, Stat* stat);
};

struct Vnode {
    VnodeType type;
    VnodeOps* ops;
    void* private_data;
    uint64_t ref_count;
    Vnode* mount_parent;
};

struct FilesystemOps {
    int (*mount)(Vnode** out_root);
    int (*unmount)(Vnode* root);
};

struct FileDescriptor {
    Vnode* vnode;
    uint64_t offset;
    uint32_t flags;
    bool used;
};

struct FdTable {
    FileDescriptor fds[CONFIG_VFS_MAX_FDS];
};

void init();

int mount(FilesystemOps* fs_ops, Vnode* target, Vnode** out_root);
int unmount(Vnode* mount_root);
int set_root(Vnode* new_root);

int resolve(Vnode* base, const char* path, Vnode** result);

int mkdir(Vnode* base, const char* path);
int open(Vnode* base, const char* path, uint32_t flags, int* out_fd);
int close(int fd);
int read(int fd, void* buf, size_t size, size_t* out_read);
int write(int fd, const void* buf, size_t size, size_t* out_written);
int readdir(int fd, Dirent* entry);
int stat(int fd, Stat* stat);
int seek(int fd, int64_t offset, int whence, uint64_t* out_pos);

void vnode_ref(Vnode* vnode);
void vnode_unref(Vnode* vnode);

} // namespace tori::vfs
