#include <tori/kernel/fs/fat.hpp>
#include <tori/kernel/allocator.hpp>
#include <tori/kernel/heap.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/string.hpp>
#include <tori/kernel/vfs.hpp>

namespace {

using namespace tori::vfs;
using namespace tori::block;

// --- BPB field offsets ---
constexpr uint16_t BPB_BYTES_PER_SECTOR      = 11;
constexpr uint16_t BPB_SECTORS_PER_CLUSTER   = 13;
constexpr uint16_t BPB_RESERVED_SECTOR_COUNT = 14;
constexpr uint16_t BPB_FAT_COUNT             = 16;
constexpr uint16_t BPB_ROOT_ENTRY_COUNT      = 17;
constexpr uint16_t BPB_TOTAL_SECTORS_16      = 19;
constexpr uint16_t BPB_MEDIA_DESCRIPTOR      = 21;
constexpr uint16_t BPB_FAT_SIZE_16           = 22;
constexpr uint16_t BPB_TOTAL_SECTORS_32      = 32;
constexpr uint16_t BPB_FAT_SIZE_32           = 36;
constexpr uint16_t BPB_ROOT_CLUSTER          = 44;

// --- Directory entry offsets ---
constexpr uint8_t DIR_NAME        = 0;
constexpr uint8_t DIR_ATTR        = 11;
constexpr uint8_t DIR_NT_RES      = 12;
constexpr uint8_t DIR_CLUSTER_HI  = 20;
constexpr uint8_t DIR_CLUSTER_LO  = 26;
constexpr uint8_t DIR_FILE_SIZE   = 28;

// --- Attribute flags ---
constexpr uint8_t ATTR_READ_ONLY  = 0x01;
constexpr uint8_t ATTR_HIDDEN     = 0x02;
constexpr uint8_t ATTR_SYSTEM     = 0x04;
constexpr uint8_t ATTR_VOLUME_ID  = 0x08;
constexpr uint8_t ATTR_DIRECTORY  = 0x10;
constexpr uint8_t ATTR_ARCHIVE    = 0x20;
constexpr uint8_t ATTR_LFN        = 0x0F;

// --- FAT entry values ---
constexpr uint32_t FAT_EOC_MIN_16  = 0xFFF8;
constexpr uint32_t FAT_EOC_MAX_16  = 0xFFFF;
constexpr uint32_t FAT_EOC_MIN_32  = 0x0FFFFFF8;
constexpr uint32_t FAT_EOC_MAX_32  = 0x0FFFFFFF;

// --- LFN ---
constexpr uint8_t LFN_LAST         = 0x40;
constexpr uint8_t LFN_MAX_ENTRIES  = 20;

// ---
struct FatInstance {
    BlockDevice* dev;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint32_t fat_size;
    uint32_t root_cluster;
    uint32_t total_sectors;
    uint32_t first_data_sector;
    uint32_t root_dir_sectors;
    bool is_fat32;

    void* fat_buffer;
    uint32_t fat_entry_count;
};

struct FatNode {
    uint32_t first_cluster;
    uint32_t file_size;
    uint8_t attributes;
    FatInstance* fs;
};

// --- Module-level state ---
static FatInstance* g_fs = nullptr;

// --- BPB parsing ---

bool read_sectors(BlockDevice* dev, uint64_t sector, void* buf, size_t count) {
    return dev->read(dev, sector, buf, count);
}

bool read_byte(BlockDevice* dev, uint64_t sector, uint16_t offset, uint8_t* out) {
    uint8_t buf[512];
    if (!read_sectors(dev, sector, buf, 1)) return false;
    *out = buf[offset];
    return true;
}

bool read_word(BlockDevice* dev, uint64_t sector, uint16_t offset, uint16_t* out) {
    uint8_t buf[512];
    if (!read_sectors(dev, sector, buf, 1)) return false;
    *out = static_cast<uint16_t>(buf[offset]) | (static_cast<uint16_t>(buf[offset + 1]) << 8);
    return true;
}

bool read_dword(BlockDevice* dev, uint64_t sector, uint16_t offset, uint32_t* out) {
    uint8_t buf[512];
    if (!read_sectors(dev, sector, buf, 1)) return false;
    *out = static_cast<uint32_t>(buf[offset])
         | (static_cast<uint32_t>(buf[offset + 1]) << 8)
         | (static_cast<uint32_t>(buf[offset + 2]) << 16)
         | (static_cast<uint32_t>(buf[offset + 3]) << 24);
    return true;
}

bool parse_bpb(FatInstance* fs) {
    if (!read_word(fs->dev, 0, BPB_BYTES_PER_SECTOR, &fs->bytes_per_sector)) return false;
    if (fs->bytes_per_sector != 512 && fs->bytes_per_sector != 1024 &&
        fs->bytes_per_sector != 2048 && fs->bytes_per_sector != 4096) return false;

    uint8_t spc;
    if (!read_byte(fs->dev, 0, BPB_SECTORS_PER_CLUSTER, &spc)) return false;
    if (spc == 0 || (spc & (spc - 1))) return false;
    fs->sectors_per_cluster = spc;

    if (!read_word(fs->dev, 0, BPB_RESERVED_SECTOR_COUNT, &fs->reserved_sector_count)) return false;
    if (!read_byte(fs->dev, 0, BPB_FAT_COUNT, &fs->fat_count)) return false;
    if (fs->fat_count == 0) return false;

    if (!read_word(fs->dev, 0, BPB_ROOT_ENTRY_COUNT, &fs->root_entry_count)) return false;

    uint16_t total_16;
    if (!read_word(fs->dev, 0, BPB_TOTAL_SECTORS_16, &total_16)) return false;
    uint32_t total_32;
    if (!read_dword(fs->dev, 0, BPB_TOTAL_SECTORS_32, &total_32)) return false;
    fs->total_sectors = (total_16 != 0) ? total_16 : total_32;

    uint16_t fat_size_16;
    if (!read_word(fs->dev, 0, BPB_FAT_SIZE_16, &fat_size_16)) return false;
    if (fat_size_16 != 0) {
        fs->is_fat32 = false;
        fs->fat_size = fat_size_16;
    } else {
        fs->is_fat32 = true;
        if (!read_dword(fs->dev, 0, BPB_FAT_SIZE_32, &fs->fat_size)) return false;
    }

    uint32_t root_dir_sectors = (static_cast<uint32_t>(fs->root_entry_count) * 32 + fs->bytes_per_sector - 1)
                                / fs->bytes_per_sector;
    fs->root_dir_sectors = root_dir_sectors;
    fs->first_data_sector = fs->reserved_sector_count + fs->fat_count * fs->fat_size + root_dir_sectors;

    if (fs->is_fat32) {
        if (!read_dword(fs->dev, 0, BPB_ROOT_CLUSTER, &fs->root_cluster)) return false;
    } else {
        fs->root_cluster = 0;
    }

    // Verify boot signature
    uint8_t sig_lo, sig_hi;
    if (!read_byte(fs->dev, 0, 510, &sig_lo)) return false;
    if (!read_byte(fs->dev, 0, 511, &sig_hi)) return false;
    if (sig_lo != 0x55 || sig_hi != 0xAA) return false;

    // Determine FAT type from cluster count
    uint32_t data_sector_count = fs->total_sectors - fs->first_data_sector;
    uint32_t cluster_count = data_sector_count / fs->sectors_per_cluster;

    if (cluster_count < 4085) {
        TORI_LOG_WARN("fat", "FAT12 detected, not supported");
        return false;
    }

    if (!fs->is_fat32 && cluster_count >= 65525) {
        // BPB says FAT16 but cluster count suggests FAT32
        TORI_LOG_WARN("fat", "FAT16 BPB but cluster count suggests FAT32");
        return false;
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "fat", "FAT type", fs->is_fat32 ? 32ULL : 16ULL);
    TORI_LOG_VALUE(tori::log::Level::Info, "fat", "bytes per sector", fs->bytes_per_sector);
    TORI_LOG_VALUE(tori::log::Level::Info, "fat", "sectors per cluster", fs->sectors_per_cluster);
    TORI_LOG_VALUE(tori::log::Level::Info, "fat", "total sectors", fs->total_sectors);
    TORI_LOG_VALUE(tori::log::Level::Info, "fat", "cluster count", cluster_count);

    return true;
}

bool read_fat_table(FatInstance* fs) {
    uint32_t entry_size = fs->is_fat32 ? 4 : 2;
    fs->fat_entry_count = (fs->fat_size * fs->bytes_per_sector) / entry_size;

    size_t fat_bytes = fs->fat_size * fs->bytes_per_sector;
    fs->fat_buffer = tori::memory::kalloc(fat_bytes, alignof(uint32_t));
    if (!fs->fat_buffer) return false;

    for (uint32_t s = 0; s < fs->fat_size; ++s) {
        uint8_t buf[512];
        if (!read_sectors(fs->dev, fs->reserved_sector_count + s, buf, 1)) {
            tori::memory::kfree(fs->fat_buffer, fat_bytes);
            fs->fat_buffer = nullptr;
            return false;
        }
        auto* dst = static_cast<uint8_t*>(fs->fat_buffer) + s * fs->bytes_per_sector;
        for (uint16_t i = 0; i < fs->bytes_per_sector; ++i) dst[i] = buf[i];
    }

    return true;
}

// --- Cluster operations ---

uint32_t cluster_to_sector(FatInstance* fs, uint32_t cluster) {
    return fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
}

uint32_t next_cluster(FatInstance* fs, uint32_t cluster) {
    if (!fs->fat_buffer) return FAT_EOC_MAX_32;
    if (fs->is_fat32) {
        auto* table = static_cast<uint32_t*>(fs->fat_buffer);
        if (cluster < fs->fat_entry_count) return table[cluster] & 0x0FFFFFFF;
    } else {
        auto* table = static_cast<uint16_t*>(fs->fat_buffer);
        if (cluster < fs->fat_entry_count) return table[cluster];
    }
    return FAT_EOC_MAX_32;
}

bool is_eoc(uint32_t value, bool is_fat32) {
    if (is_fat32) return value >= FAT_EOC_MIN_32;
    return value >= FAT_EOC_MIN_16;
}

// Read data from a file or directory given its cluster chain.
size_t read_from_chain(FatInstance* fs, uint32_t start_cluster, uint64_t offset,
                       void* buf, size_t size) {
    if (size == 0) return 0;

    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t current_cluster = start_cluster;

    // Skip to the cluster containing offset
    uint64_t skip = offset;
    while (skip >= cluster_size) {
        if (is_eoc(current_cluster, fs->is_fat32)) return 0;
        current_cluster = next_cluster(fs, current_cluster);
        if (current_cluster == 0 || is_eoc(current_cluster, fs->is_fat32)) return 0;
        skip -= cluster_size;
    }

    size_t done = 0;
    while (done < size) {
        if (current_cluster < 2 || is_eoc(current_cluster, fs->is_fat32)) break;

        uint32_t sector = cluster_to_sector(fs, current_cluster);
        uint32_t cluster_off = static_cast<uint32_t>(skip);
        uint32_t chunk = cluster_size - cluster_off;
        if (chunk > size - done) chunk = static_cast<uint32_t>(size - done);

        auto* dst = static_cast<uint8_t*>(buf) + done;

        // Read sectors
        uint32_t start_sector_off = cluster_off / fs->bytes_per_sector;
        uint32_t end_sector_off = (cluster_off + chunk + fs->bytes_per_sector - 1) / fs->bytes_per_sector;

        for (uint32_t s = start_sector_off; s < end_sector_off; ++s) {
            uint8_t sec_buf[512];
            if (!read_sectors(fs->dev, sector + s, sec_buf, 1)) return done;

            uint32_t sec_start = (s == start_sector_off) ? (cluster_off % fs->bytes_per_sector) : 0;
            uint32_t sec_end = (s == end_sector_off - 1)
                ? (cluster_off + chunk) % fs->bytes_per_sector
                : fs->bytes_per_sector;
            if (sec_end == 0) sec_end = fs->bytes_per_sector;

            for (uint32_t i = sec_start; i < sec_end; ++i) {
                *dst++ = sec_buf[i];
            }
        }

        done += chunk;
        skip = 0;

        current_cluster = next_cluster(fs, current_cluster);
    }

    return done;
}

// Read from FAT16 fixed root directory.
size_t read_fat16_root(FatInstance* fs, uint64_t offset, void* buf, size_t size) {
    uint32_t root_size = fs->root_entry_count * 32;
    if (offset >= root_size) return 0;
    if (offset + size > root_size) size = static_cast<size_t>(root_size - offset);

    uint32_t root_start_sector = fs->reserved_sector_count + fs->fat_count * fs->fat_size;
    uint32_t sector_off = static_cast<uint32_t>(offset / fs->bytes_per_sector);
    uint32_t byte_off = static_cast<uint32_t>(offset % fs->bytes_per_sector);

    size_t done = 0;
    while (done < size) {
        uint8_t sec_buf[512];
        if (!read_sectors(fs->dev, root_start_sector + sector_off, sec_buf, 1)) return done;

        uint32_t chunk = fs->bytes_per_sector - byte_off;
        if (chunk > size - done) chunk = static_cast<uint32_t>(size - done);

        auto* dst = static_cast<uint8_t*>(buf) + done;
        for (uint32_t i = 0; i < chunk; ++i) dst[i] = sec_buf[byte_off + i];

        done += chunk;
        ++sector_off;
        byte_off = 0;
    }

    return done;
}

size_t read_from_location(FatInstance* fs, uint32_t cluster, bool is_fat16_root,
                          uint64_t offset, void* buf, size_t size) {
    if (is_fat16_root) return read_fat16_root(fs, offset, buf, size);
    return read_from_chain(fs, cluster, offset, buf, size);
}

// --- Directory entry helpers ---

uint8_t sfn_checksum(const uint8_t* entry) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + entry[i];
    }
    return sum;
}

void extract_sfn_name(const uint8_t* entry, char* name, size_t max_len) {
    if (max_len == 0) return;

    uint8_t name_bytes[11];
    for (int i = 0; i < 11; ++i) name_bytes[i] = entry[DIR_NAME + i];

    if (name_bytes[0] == 0x05) name_bytes[0] = 0xE5;

    size_t pos = 0;
    for (int i = 0; i < 8 && name_bytes[i] != ' '; ++i) {
        if (pos < max_len - 1) name[pos++] = static_cast<char>(name_bytes[i]);
    }

    bool has_ext = name_bytes[8] != ' ';
    if (has_ext) {
        if (pos < max_len - 1) name[pos++] = '.';
        for (int i = 8; i < 11 && name_bytes[i] != ' '; ++i) {
            if (pos < max_len - 1) name[pos++] = static_cast<char>(name_bytes[i]);
        }
    }

    // Apply NT case bits: if bit 4 set, lowercase base; if bit 3 set, lowercase ext
    uint8_t nt_res = entry[DIR_NT_RES];
    if (nt_res & 0x08) {
        // lowercase extension
        for (size_t i = pos; i > 0; --i) {
            if (name[i - 1] >= 'A' && name[i - 1] <= 'Z') name[i - 1] += 0x20;
            if (name[i - 1] == '.') break;
        }
    }
    if (nt_res & 0x10) {
        // lowercase base name
        for (size_t i = 0; i < pos && name[i] != '.'; ++i) {
            if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 0x20;
        }
    }

    name[pos] = '\0';
}

bool is_end_marker(const uint8_t* entry) {
    return entry[DIR_NAME] == 0x00;
}

bool is_deleted(const uint8_t* entry) {
    return entry[DIR_NAME] == 0xE5;
}

bool is_lfn_entry(const uint8_t* entry) {
    return entry[DIR_ATTR] == ATTR_LFN;
}

bool is_regular_entry(const uint8_t* entry) {
    if (is_deleted(entry) || is_end_marker(entry) || is_lfn_entry(entry)) return false;
    uint8_t attr = entry[DIR_ATTR];
    if (attr == ATTR_VOLUME_ID) return false;
    if (attr & ATTR_VOLUME_ID) return false; // Volume label
    return true;
}

bool name_matches_sfn(const uint8_t* entry, const char* name) {
    char sfn[13];
    extract_sfn_name(entry, sfn, sizeof(sfn));

    for (size_t i = 0; ; ++i) {
        char a = name[i];
        char b = sfn[i];
        if (a >= 'A' && a <= 'Z') a += 0x20;
        if (b >= 'A' && b <= 'Z') b += 0x20;
        if (a == '\0' && b == '\0') return true;
        if (a != b) return false;
    }
}

void reconstruct_lfn_name(const uint8_t* lfn_entries, int count, char* name, size_t max_len) {
    size_t pos = 0;

    for (int i = count - 1; i >= 0; --i) {
        const uint8_t* e = lfn_entries + i * 32;

        auto read_char = [&](uint16_t off) -> uint16_t {
            return static_cast<uint16_t>(e[off]) | (static_cast<uint16_t>(e[off + 1]) << 8);
        };

        // Characters 1-5 at offsets 1,3,5,7,9 (each 2 bytes)
        for (int c = 0; c < 5; ++c) {
            uint16_t uc = read_char(1 + c * 2);
            if (uc == 0 || uc == 0xFFFF) break;
            if (pos < max_len - 1) name[pos++] = (uc < 0x80) ? static_cast<char>(uc) : '?';
        }

        // Characters 6-11 at offsets 14,16,18,20,22,24
        for (int c = 0; c < 6; ++c) {
            uint16_t uc = read_char(14 + c * 2);
            if (uc == 0 || uc == 0xFFFF) break;
            if (pos < max_len - 1) name[pos++] = (uc < 0x80) ? static_cast<char>(uc) : '?';
        }

        // Characters 12-13 at offsets 28,30
        for (int c = 0; c < 2; ++c) {
            uint16_t uc = read_char(28 + c * 2);
            if (uc == 0 || uc == 0xFFFF) break;
            if (pos < max_len - 1) name[pos++] = (uc < 0x80) ? static_cast<char>(uc) : '?';
        }
    }

    name[pos] = '\0';
}

// Find a directory entry by name. Returns FatNode info.
int find_entry(FatInstance* fs, uint32_t dir_cluster, bool is_fat16_root,
               const char* name, FatNode* out_node) {
    if (!name || name[0] == '\0') return E_NOT_FOUND;

    uint8_t cluster_buf[4096]; // max cluster size: 4096 (8 * 512)
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    if (cluster_size > sizeof(cluster_buf)) return E_IO;

    // Dot and dot-dot: skip
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        return E_NOT_FOUND;
    }

    if (is_fat16_root) {
        uint32_t root_size = fs->root_entry_count * 32;
        uint64_t offset = 0;

        while (offset + 32 <= root_size) {
            uint8_t entry[32];
            size_t n = read_fat16_root(fs, offset, entry, 32);
            if (n < 32) break;

            if (is_end_marker(entry)) break;

            if (is_regular_entry(entry) && name_matches_sfn(entry, name)) {
                out_node->first_cluster = (static_cast<uint32_t>(entry[DIR_CLUSTER_HI]) << 16)
                                        | static_cast<uint32_t>(entry[DIR_CLUSTER_LO]);
                out_node->file_size = static_cast<uint32_t>(entry[DIR_FILE_SIZE])
                                    | (static_cast<uint32_t>(entry[DIR_FILE_SIZE + 1]) << 8)
                                    | (static_cast<uint32_t>(entry[DIR_FILE_SIZE + 2]) << 16)
                                    | (static_cast<uint32_t>(entry[DIR_FILE_SIZE + 3]) << 24);
                out_node->attributes = entry[DIR_ATTR];
                out_node->fs = fs;
                return 0;
            }

            offset += 32;
        }
    } else {
        uint32_t current = dir_cluster;

        while (current >= 2 && !is_eoc(current, fs->is_fat32)) {
            uint32_t sector = cluster_to_sector(fs, current);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; ++s) {
                uint8_t sec_buf[512];
                if (!read_sectors(fs->dev, sector + s, sec_buf, 1)) return E_IO;

                for (uint16_t off = 0; off + 32 <= fs->bytes_per_sector; off += 32) {
                    const uint8_t* entry = sec_buf + off;
                    if (is_end_marker(entry)) return E_NOT_FOUND;
                    if (!is_regular_entry(entry)) continue;

                    if (name_matches_sfn(entry, name)) {
                        out_node->first_cluster = (static_cast<uint32_t>(entry[DIR_CLUSTER_HI]) << 16)
                                                | static_cast<uint32_t>(entry[DIR_CLUSTER_LO]);
                        out_node->file_size = static_cast<uint32_t>(entry[DIR_FILE_SIZE])
                                            | (static_cast<uint32_t>(entry[DIR_FILE_SIZE + 1]) << 8)
                                            | (static_cast<uint32_t>(entry[DIR_FILE_SIZE + 2]) << 16)
                                            | (static_cast<uint32_t>(entry[DIR_FILE_SIZE + 3]) << 24);
                        out_node->attributes = entry[DIR_ATTR];
                        out_node->fs = fs;
                        return 0;
                    }
                }
            }

            current = next_cluster(fs, current);
        }
    }

    return E_NOT_FOUND;
}

// Read the next directory entry from a FAT directory, skipping LFN entries.
// offset is a byte position into the directory.
int read_next_entry(FatInstance* fs, uint32_t dir_cluster, bool is_fat16_root,
                    uint64_t* io_offset, Dirent* out_entry) {
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t max_size = is_fat16_root ? fs->root_entry_count * 32 : 0xFFFFFFFF;

    uint64_t offset = *io_offset;

    while (true) {
        uint8_t entry[32];
        size_t n;
        if (is_fat16_root) {
            n = read_fat16_root(fs, offset, entry, 32);
        } else {
            n = read_from_chain(fs, dir_cluster, offset, entry, 32);
        }

        if (n < 32) return E_NOT_FOUND;

        if (is_end_marker(entry)) return E_NOT_FOUND;

        if (is_deleted(entry) || is_lfn_entry(entry)) {
            offset += 32;
            continue;
        }

        uint8_t attr = entry[DIR_ATTR];
        if (attr == ATTR_VOLUME_ID || (attr & ATTR_VOLUME_ID)) {
            offset += 32;
            continue;
        }

        // Found a regular entry
        extract_sfn_name(entry, out_entry->name, sizeof(out_entry->name));

        // Skip dot entries
        if (out_entry->name[0] == '.' && (out_entry->name[1] == '\0' ||
            (out_entry->name[1] == '.' && out_entry->name[2] == '\0'))) {
            offset += 32;
            continue;
        }

        out_entry->type = (attr & ATTR_DIRECTORY) ? VnodeType::Directory : VnodeType::File;
        *io_offset = offset + 32;
        return 0;
    }
}

// --- VnodeOps ---

int fat_lookup(Vnode* dir, const char* name, Vnode** result) {
    auto* parent = static_cast<FatNode*>(dir->private_data);
    FatInstance* fs = parent->fs;

    FatNode child;
    int err = find_entry(fs, parent->first_cluster,
                         (!fs->is_fat32 && parent->first_cluster == 0),
                         name, &child);
    if (err < 0) return err;

    auto* vnode = static_cast<Vnode*>(tori::memory::kalloc(sizeof(Vnode), alignof(Vnode)));
    if (!vnode) return E_NO_SPACE;

    auto* node_data = static_cast<FatNode*>(tori::memory::kalloc(sizeof(FatNode), alignof(FatNode)));
    if (!node_data) {
        tori::memory::kfree(vnode, sizeof(Vnode));
        return E_NO_SPACE;
    }

    *node_data = child;
    vnode->type = (child.attributes & ATTR_DIRECTORY) ? VnodeType::Directory : VnodeType::File;
    vnode->ops = dir->ops;
    vnode->private_data = node_data;
    vnode->ref_count = 1;
    vnode->mount_parent = nullptr;

    *result = vnode;
    return 0;
}

int fat_read(Vnode* node, uint64_t offset, void* buf, size_t size, size_t* out_read) {
    if (node->type != VnodeType::File) return E_IS_DIR;
    auto* fn = static_cast<FatNode*>(node->private_data);
    FatInstance* fs = fn->fs;

    if (offset >= fn->file_size) {
        *out_read = 0;
        return 0;
    }

    if (offset + size > fn->file_size) {
        size = static_cast<size_t>(fn->file_size - offset);
    }

    size_t n = read_from_chain(fs, fn->first_cluster, offset, buf, size);
    *out_read = n;
    return 0;
}

int fat_readdir(Vnode* dir, uint64_t offset, Dirent* entry, uint64_t* out_offset) {
    if (dir->type != VnodeType::Directory) return E_NOT_DIR;
    auto* fn = static_cast<FatNode*>(dir->private_data);
    FatInstance* fs = fn->fs;

    bool is_root16 = !fs->is_fat32 && fn->first_cluster == 0;
    return read_next_entry(fs, fn->first_cluster, is_root16, &offset, entry)
        ? E_NOT_FOUND
        : (*out_offset = offset, 0);
}

int fat_stat(Vnode* node, Stat* stat) {
    auto* fn = static_cast<FatNode*>(node->private_data);
    stat->type = node->type;
    stat->size = fn->file_size;
    return 0;
}

int fat_stub_create(Vnode*, const char*, Vnode**) { return E_INVALID; }
int fat_stub_mkdir(Vnode*, const char*) { return E_INVALID; }
int fat_stub_rmdir(Vnode*, const char*) { return E_INVALID; }
int fat_stub_unlink(Vnode*, const char*) { return E_INVALID; }
int fat_stub_write(Vnode*, uint64_t, const void*, size_t, size_t*) { return E_INVALID; }

VnodeOps fat_vnode_ops = {
    .lookup  = fat_lookup,
    .create  = fat_stub_create,
    .mkdir   = fat_stub_mkdir,
    .rmdir   = fat_stub_rmdir,
    .unlink  = fat_stub_unlink,
    .read    = fat_read,
    .write   = fat_stub_write,
    .readdir = fat_readdir,
    .stat    = fat_stat,
};

// --- FilesystemOps ---

int fat_mount(Vnode** out_root) {
    if (!g_fs) return E_IO;

    auto* vnode = static_cast<Vnode*>(tori::memory::kalloc(sizeof(Vnode), alignof(Vnode)));
    if (!vnode) return E_NO_SPACE;

    auto* fn = static_cast<FatNode*>(tori::memory::kalloc(sizeof(FatNode), alignof(FatNode)));
    if (!fn) {
        tori::memory::kfree(vnode, sizeof(Vnode));
        return E_NO_SPACE;
    }

    fn->fs = g_fs;
    if (g_fs->is_fat32) {
        fn->first_cluster = g_fs->root_cluster;
        fn->file_size = 0;
    } else {
        fn->first_cluster = 0; // sentinel for FAT16 fixed root
        fn->file_size = g_fs->root_entry_count * 32;
    }
    fn->attributes = ATTR_DIRECTORY;

    vnode->type = VnodeType::Directory;
    vnode->ops = &fat_vnode_ops;
    vnode->private_data = fn;
    vnode->ref_count = 1;
    vnode->mount_parent = nullptr;

    *out_root = vnode;
    return 0;
}

int fat_unmount(Vnode* root) {
    if (root->private_data) {
        tori::memory::kfree(root->private_data, sizeof(FatNode));
    }
    tori::memory::kfree(root, sizeof(Vnode));
    return 0;
}

} // namespace

// --- Public API ---

namespace tori::fat {

int init(BlockDevice* dev) {
    if (g_fs) {
        TORI_LOG_WARN("fat", "already initialized");
        return -1;
    }

    auto* fs = static_cast<FatInstance*>(tori::memory::kalloc(sizeof(FatInstance), alignof(FatInstance)));
    if (!fs) return -1;

    tori::memory::zero_memory(fs, sizeof(FatInstance));
    fs->dev = dev;

    if (!parse_bpb(fs)) {
        tori::memory::kfree(fs, sizeof(FatInstance));
        return -2;
    }

    if (!read_fat_table(fs)) {
        tori::memory::kfree(fs, sizeof(FatInstance));
        return -3;
    }

    g_fs = fs;
    TORI_LOG_INFO("fat", "FAT filesystem driver initialized");
    return 0;
}

void shutdown() {
    if (!g_fs) return;
    if (g_fs->fat_buffer) {
        size_t fat_bytes = g_fs->fat_size * g_fs->bytes_per_sector;
        tori::memory::kfree(g_fs->fat_buffer, fat_bytes);
    }
    tori::memory::kfree(g_fs, sizeof(FatInstance));
    g_fs = nullptr;
}

vfs::FilesystemOps fs_ops = {
    .mount   = fat_mount,
    .unmount = fat_unmount,
};

} // namespace tori::fat
