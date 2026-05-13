# FAT16/32 Filesystem Driver

## Goal

Replace per-file Limine module loading with a single FAT32 boot module image
and implement a proper VFS-plugin FAT16/32 driver.

## Architecture

```
Build:   iso/rootfs/ → mkfs.fat + mcopy → iso/boot/rootfs.fat
Boot:    rootfs.fat (boot module) → MemBlockDevice → FAT driver → VFS mount
         → walk FAT dir tree → populate RamFS → overlayFS (lower=populated RamFS, upper=RW RamFS)
```

The FAT driver is a full VFS filesystem (`FilesystemOps` + `VnodeOps`), not a
one-shot extractor. It is tested on every boot because the initramfs payload
is delivered as a FAT image.

## Block Device Layer

New file: `kernel/include/tori/kernel/block_device.hpp`

```cpp
struct BlockDevice {
    uint64_t sector_count;
    uint16_t sector_size;   // always 512 for FAT
    bool (*read)(BlockDevice* dev, uint64_t sector, void* buf, size_t count);
    bool (*write)(BlockDevice* dev, uint64_t sector, const void* buf, size_t count);
    void* private_data;
};
```

New file: `kernel/src/block/memblock.cpp` — wraps a `(data, size)` buffer as a
read-only BlockDevice. The `read` callback copies from the buffer. `write` is
null (returns false). This is the backend until a real disk driver exists.

## FAT Driver

New files:
- `kernel/include/tori/kernel/fs/fat.hpp`  — `fat::init(BlockDevice*)`, `fat::fs_ops`
- `kernel/src/fs/fat.cpp` — full implementation

### State

`fat::init()` allocates a `FatInstance` on the heap, stores it in a module-local
static. The instance holds:

- The BlockDevice pointer
- Parsed BPB fields (sector/cluster geometry, FAT location, root cluster)
- A heap-cached copy of the FAT table (FAT16: `uint16_t[]`, FAT32: `uint32_t[] & 0x0FFFFFFF`)
- `bool is_fat32`

### Vnode private_data

```cpp
struct FatNode {
    uint32_t first_cluster;
    uint32_t file_size;
    uint8_t attributes;
};
```

Root directory Vnode has `first_cluster = root_cluster` (FAT32) or `0` (FAT16
fixed root, where `cluster == 0` is a sentinel meaning "read from root dir
sectors").

### FilesystemOps

| Op | Behavior |
|----|----------|
| `mount` | Return root Vnode with FatNode pointing at root directory |
| `unmount` | Free cached FAT table, free FatInstance, release BlockDevice |

### VnodeOps

| Op | Behavior |
|----|----------|
| `lookup(dir, name, result)` | Walk directory entries matching SFN or resolved LFN. Return child Vnode or error |
| `read(file, offset, buf, size, out)` | Walk cluster chain from FatNode, copy data into buf |
| `readdir(dir, offset, entry, out_offset)` | Iterate directory entries, skip deleted/LFN/dot, return SFN |
| `stat(node, stat)` | Return size, mode from FatNode attributes |
| `create/write/mkdir/rmdir/unlink` | Stub `E_INVALID` (read-only for now) |

### LFN Handling

LFN entries (attribute `0x0F`) form a chain preceding the SFN entry. Checksum
in each LFN entry is verified against the SFN checksum. Name is reconstructed
from UCS-2 fragments. For `lookup`: match against LFN or SFN. For `readdir`:
return SFN only (to keep the Dirent simple; LFN exposure is future work).

### FAT Cluster Walk

```cpp
uint32_t next_cluster(FatInstance* fs, uint32_t cluster);
bool is_eoc(uint32_t value, bool is_fat32);
```

Returns sectors per cluster starting from `first_data_sector + (cluster - 2) * spt`.

## Build Integration

### `scripts/mkrootfs.sh` (or inline CMake commands)

```sh
IMG=iso/boot/rootfs.fat
dd if=/dev/zero of=$IMG bs=1K count=4096
mkfs.fat -F 32 $IMG
mcopy -i $IMG iso/rootfs/* ::/
```

This runs as a CMake `add_custom_command` producing `rootfs.fat`, which
`tori_iso_root` depends on and copies to the ISO root.

### `iso/boot/limine.conf`

```diff
- module_path: boot():/rootfs/hello.txt
+ module_path: boot():/boot/rootfs.fat
```

### `kernel/src/kernel/main.cpp`

Replaces the per-module loop with:

1. Find the `rootfs.fat` boot module by scanning `owned_boot_info.modules[]`
2. Create a `MemBlockDevice` backed by the module address/size
3. Call `fat::init(memblock)` → parses BPB, caches FAT
4. Mount FAT via `fat::fs_ops.mount()` → get FAT root Vnode
5. Walk FAT directory tree recursively, create each file/dir in RamFS
6. Proceed to overlayFS setup (lower = populated RamFS, upper = RW RamFS)

## Files Changed

| File | Action |
|------|--------|
| `kernel/include/tori/kernel/block_device.hpp` | New |
| `kernel/src/block/memblock.cpp` | New |
| `kernel/include/tori/kernel/fs/fat.hpp` | New |
| `kernel/src/fs/fat.cpp` | New |
| `iso/boot/limine.conf` | Modify module_path |
| `kernel/src/kernel/main.cpp` | Replace module loop with FAT init |
| `iso/rootfs/` | Unchanged (source for FAT image) |
| `kernel/CMakeLists.txt` | Add rootfs.fat build command |

## Future

- Write support (cluster allocation, FAT update, directory entry creation)
- Real block device backend (AHCI / UEFI BlockIo) — replace `MemBlockDevice` with a disk-backed device, no FAT driver changes needed
- LFN exposure in `readdir` results
- FAT12 support (trivial but deferred — no target uses it)
