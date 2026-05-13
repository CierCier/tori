# Filesystem Milestone (6.5)

## Summary

Insert between Milestone 6 (Scheduling) and Milestone 7 (User Boundary). Provides an in-kernel filesystem stack: VFS abstraction layer, RamFS for in-memory trees, overlayFS for N-layer stacking with whiteouts, and Limine boot module plumbing for initramfs loading. FAT16/32 drivers deferred to a later sub-milestone.

## Architecture

```
  kernel / userspace
        |
   POSIX fd API (open/read/write/close/readdir/stat)
        |
    VFS Core (vnode ops, mount table, fd table, path resolution)
        |
    +-----------+-----------+-------------+
    |           |           |             |
  RamFS     overlayFS    FAT16/32      future FS
    |           |         (later)
    |      N layers
    |   (RO lower +
    |    RW upper)
    |
  PMM pages
```

## Build Order

1. VFS Core — mount table, fd table, vnode operations, path resolution
2. RamFS — in-memory FS backed by PMM pages, implements all VnodeOps
3. Limine module plumbing — collect module data into BootInfo, map into VAS
4. overlayFS — N-layer stacking with whiteout support, merged readdir
5. Integration — mount root RamFS, load initramfs, overlay writable layer

## VFS Core

### Data structures

```cpp
namespace tori::vfs {

enum class VnodeType { File, Directory, Symlink };

struct Stat {
    VnodeType type;
    uint64_t size;
};

struct Dirent {
    char name[256];
    VnodeType type;
};

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

} // namespace tori::vfs
```

### API

```cpp
void init();
int mount(FilesystemOps* fs_ops, Vnode* target, Vnode** out_root);
int unmount(Vnode* mount_root);
int resolve(Vnode* base, const char* path, Vnode** result);

int open(Vnode* base, const char* path, uint32_t flags, int* out_fd);
int close(int fd);
int read(int fd, void* buf, size_t size, size_t* out_read);
int write(int fd, const void* buf, size_t size, size_t* out_written);
int readdir(int fd, Dirent* entry);
int stat(int fd, Stat* stat);
int seek(int fd, int64_t offset, int whence, uint64_t* out_pos);

void vnode_ref(Vnode* vnode);
void vnode_unref(Vnode* vnode);
```

## RamFS

Simple in-memory filesystem implementing VnodeOps.

- **Directory**: stored as a linked list or array of (name, Vnode*) entries, sorted for readdir stability
- **File**: data stored in a list of 4 KiB page chunks (allocated via PMM)
- **Data structure**: `RamfsNode` with type, child list, data pages, size, refcount
- Files created empty, grown via `write` (allocate pages on demand)
- `unlink`/`rmdir` frees pages and node memory

## overlayFS

N-layer stacking with whiteout support.

- **Layers**: array of (Vnode* root, bool writable)
- **lookup**: scan layers top to bottom, return first match
- **readdir**: iterate all layers, merge entries (top name wins), skip whiteouts
- **create**: topmost writable layer only
- **unlink**: create whiteout on topmost writable layer (a special entry marking "deleted here")
- **whiteout**: stored as a regular file with a reserved name prefix on each layer

## Limine Module Plumbing

- Add module list fields to `BootInfo`:
  ```cpp
  struct BootModule {
      uint64_t address;
      uint64_t size;
      char path[256];
  };
  
  struct BootInfo {
      // ...existing fields...
      BootModule* modules;
      uint64_t module_count;
  };
  ```
- In `limine_entry()`, collect `limine_file` array into Tori-owned `BootModule` array (allocated via heap)
- Map each module's physical pages into VAS (they may be outside the HHDM range)
- After VFS/RamFS init, iterate modules and load into RamFS

## Config

```c
#define CONFIG_VFS_MAX_MOUNTS    16
#define CONFIG_VFS_MAX_FDS       256
#define CONFIG_RAMFS_PREALLOC_PAGES 64
```

## Files

| File | Change |
|------|--------|
| `kernel/include/tori/kernel/vfs.hpp` | New — VFS types and API |
| `kernel/src/fs/vfs.cpp` | New — VFS core implementation |
| `kernel/include/tori/kernel/fs/ramfs.hpp` | New — RamFS header |
| `kernel/src/fs/ramfs.cpp` | New — RamFS implementation |
| `kernel/include/tori/kernel/fs/overlayfs.hpp` | New — overlayFS header |
| `kernel/src/fs/overlayfs.cpp` | New — overlayFS implementation |
| `kernel/include/tori/kernel/boot_info.hpp` | Add BootModule array |
| `kernel/src/boot/limine/entry.cpp` | Collect module data into BootInfo |
| `kernel/src/kernel/main.cpp` | Add VFS/RamFS/overlayFS init |
| `kernel/config.h` | Add VFS/RamFS config constants |
