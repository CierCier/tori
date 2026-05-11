#include <tori/kernel/heap.hpp>

#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/vmem_layout.hpp>

namespace {

constexpr uint64_t allocated_flag = 1;
constexpr uint64_t size_mask = ~allocated_flag;
constexpr size_t min_block_size = 32;
constexpr size_t grow_page_count = 4;
constexpr size_t max_regions = 16;

struct BlockHeader {
    uint64_t size_alloc;
};

struct Footer {
    uint64_t size_alloc;
};

struct HeapRegion {
    uint8_t* base;
    uint8_t* limit;
};

HeapRegion regions[max_regions];
size_t region_count = 0;

// Free list: blocks sorted by address, singly linked via FreeNode stored
// in the payload area of free blocks.
struct FreeNode {
    FreeNode* next;
};

FreeNode* free_list = nullptr;
uint64_t heap_backing_pages = 0;
uint64_t heap_failed_allocations = 0;

BlockHeader* block_header(const void* ptr) {
    auto* hdr = static_cast<const BlockHeader*>(ptr);
    return const_cast<BlockHeader*>(hdr - 1);
}

void* block_payload(BlockHeader* hdr) {
    return hdr + 1;
}

uint64_t block_size(const BlockHeader* hdr) {
    return hdr->size_alloc & size_mask;
}

bool block_allocated(const BlockHeader* hdr) {
    return (hdr->size_alloc & allocated_flag) != 0;
}

BlockHeader* next_block(const BlockHeader* hdr) {
    auto* bytes = reinterpret_cast<const uint8_t*>(hdr);
    return reinterpret_cast<BlockHeader*>(const_cast<uint8_t*>(bytes + block_size(hdr)));
}

Footer* block_footer(BlockHeader* hdr) {
    auto* bytes = reinterpret_cast<uint8_t*>(hdr);
    return reinterpret_cast<Footer*>(bytes + block_size(hdr) - sizeof(Footer));
}

void set_block(BlockHeader* hdr, uint64_t size, bool allocated) {
    hdr->size_alloc = size | (allocated ? allocated_flag : 0);
    block_footer(hdr)->size_alloc = hdr->size_alloc;
}

uint64_t read_shift(const void* ptr) {
    auto* shift_bytes = static_cast<const uint8_t*>(ptr) - sizeof(uint64_t);
    const uint64_t* shift_ptr = reinterpret_cast<const uint64_t*>(shift_bytes);
    return *shift_ptr;
}

void write_shift(void* ptr, uint64_t shift) {
    auto* shift_bytes = static_cast<uint8_t*>(ptr) - sizeof(uint64_t);
    auto* shift_ptr = reinterpret_cast<uint64_t*>(shift_bytes);
    *shift_ptr = shift;
}

void* payload_from_user_ptr(void* ptr) {
    const uint64_t shift = read_shift(ptr);
    return static_cast<uint8_t*>(ptr) - shift;
}

BlockHeader* header_from_user_ptr(void* ptr) {
    return block_header(payload_from_user_ptr(ptr));
}

void remove_from_free_list(FreeNode* target) {
    FreeNode** prev = &free_list;
    while (*prev != nullptr && *prev != target) {
        prev = &(*prev)->next;
    }
    if (*prev != nullptr) {
        *prev = target->next;
    }
}

void add_to_free_list(FreeNode* node) {
    FreeNode** prev = &free_list;
    while (*prev != nullptr && *prev < node) {
        prev = &(*prev)->next;
    }
    node->next = *prev;
    *prev = node;
}

bool region_is_empty(uint64_t size) {
    return size < sizeof(BlockHeader) + sizeof(uint64_t) + sizeof(Footer) + 1;
}

void init_region_as_free_block(uint8_t* base, uint64_t size) {
    if (region_is_empty(size)) {
        return;
    }
    auto* hdr = reinterpret_cast<BlockHeader*>(base);
    set_block(hdr, size, false);
    auto* node = static_cast<FreeNode*>(block_payload(hdr));
    add_to_free_list(node);
}

bool try_add_region(uint8_t* base, uint8_t* limit) {
    if (region_count >= max_regions) {
        return false;
    }
    regions[region_count].base = base;
    regions[region_count].limit = limit;
    ++region_count;
    return true;
}

bool grow_heap() {
    const uint64_t phys = tori::memory::pmm::alloc_pages(grow_page_count);
    if (phys == tori::memory::pmm::invalid_physical_address) {
        return false;
    }

    auto* virt = static_cast<uint8_t*>(tori::memory::address::physical_to_virtual(phys));
    const uint64_t region_size = grow_page_count * tori::memory::pmm::page_size;

    if (!try_add_region(virt, virt + region_size)) {
        tori::memory::pmm::free_pages(phys, grow_page_count);
        return false;
    }

    heap_backing_pages += grow_page_count;
    init_region_as_free_block(virt, region_size);
    return true;
}

uintptr_t align_up(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

namespace tori::memory::heap {

void init() {
    region_count = 0;
    free_list = nullptr;
    heap_backing_pages = 0;
    heap_failed_allocations = 0;

    TORI_LOG_INFO("heap", "early kernel heap initialized");
}

void* alloc(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }

    if (alignment < alignof(uint64_t)) {
        alignment = alignof(uint64_t);
    }

    FreeNode** prev_link = &free_list;
    FreeNode* node = free_list;

    while (node != nullptr) {
        auto* hdr = block_header(node);
        const uint64_t bsize = block_size(hdr);
        auto* block_start = reinterpret_cast<uint8_t*>(hdr);

        // Compute where the user pointer will go (with alignment)
        // Layout: [BlockHeader][shift][padding?][user data][Footer]
        //          ^--hdr      ^--payload       ^--user_ptr
        //
        // user_ptr = align_up(payload + sizeof(shift), alignment)
        // total_used = (user_ptr - block_start) + size + sizeof(Footer)
        uintptr_t payload_addr = reinterpret_cast<uintptr_t>(block_payload(hdr));
        uintptr_t user_ptr = align_up(payload_addr + sizeof(uint64_t), alignment);
        uint64_t total_used = static_cast<uint64_t>((user_ptr - reinterpret_cast<uintptr_t>(block_start)) + size + sizeof(Footer));

        if (total_used < min_block_size) {
            total_used = min_block_size;
        }

        if (total_used <= bsize) {
            remove_from_free_list(node);

            uint64_t remaining = bsize - total_used;
            if (remaining >= min_block_size) {
                // Split: allocate from the start, leave remainder as free
                set_block(hdr, total_used, true);

                auto* remainder_hdr = reinterpret_cast<BlockHeader*>(block_start + total_used);
                set_block(remainder_hdr, remaining, false);
                add_to_free_list(static_cast<FreeNode*>(block_payload(remainder_hdr)));
            } else {
                // Use the whole block
                set_block(hdr, bsize, true);
            }

            // Store the shift: distance from payload to user pointer
            uint64_t shift = static_cast<uint64_t>(user_ptr - payload_addr);
            write_shift(reinterpret_cast<void*>(user_ptr), shift);
            return reinterpret_cast<void*>(user_ptr);
        }

        prev_link = &node->next;
        node = node->next;
    }

    // No suitable block found - grow the heap and retry
    if (!grow_heap()) {
        ++heap_failed_allocations;
        return nullptr;
    }

    return alloc(size, alignment);
}

void free(void* pointer) {
    if (pointer == nullptr) {
        return;
    }

    if (!contains(pointer)) {
        return;
    }

    auto* hdr = header_from_user_ptr(pointer);
    if (!block_allocated(hdr)) {
        return;
    }

    const uint64_t bsize = block_size(hdr);
    set_block(hdr, bsize, false);

    // Find which region this block belongs to (for boundary checking in coalesce)
    const uint8_t* region_start = nullptr;
    for (size_t i = 0; i < region_count; ++i) {
        if (reinterpret_cast<const uint8_t*>(hdr) >= regions[i].base &&
            reinterpret_cast<const uint8_t*>(hdr) < regions[i].limit) {
            region_start = regions[i].base;
            break;
        }
    }

    // Coalesce with next block if it exists and is free
    auto* next = next_block(hdr);
    bool next_in_region = false;
    for (size_t i = 0; i < region_count; ++i) {
        if (reinterpret_cast<const uint8_t*>(next) >= regions[i].base &&
            reinterpret_cast<const uint8_t*>(next) < regions[i].limit) {
            next_in_region = true;
            break;
        }
    }
    if (next_in_region && !block_allocated(next)) {
        remove_from_free_list(static_cast<FreeNode*>(block_payload(next)));
        set_block(hdr, bsize + block_size(next), false);
    }

    // Coalesce with previous block if it exists and is free
    if (region_start != nullptr &&
        reinterpret_cast<const uint8_t*>(hdr) > region_start + sizeof(Footer)) {
        // Read previous block's footer (footer at hdr - sizeof(Footer))
        auto* prev_footer_bytes = reinterpret_cast<const uint8_t*>(hdr) - sizeof(Footer);
        auto* prev_footer = reinterpret_cast<const Footer*>(prev_footer_bytes);
        const uint64_t prev_size = prev_footer->size_alloc & size_mask;
        if (prev_size > 0) {
            auto* prev = reinterpret_cast<BlockHeader*>(reinterpret_cast<uint8_t*>(hdr) - prev_size);
            auto* region_start_hdr = reinterpret_cast<BlockHeader*>(const_cast<uint8_t*>(region_start));
            if (prev >= region_start_hdr && !block_allocated(prev)) {
                remove_from_free_list(static_cast<FreeNode*>(block_payload(prev)));
                set_block(prev, block_size(prev) + block_size(hdr), false);
                hdr = prev;
            }
        }
    }

    add_to_free_list(static_cast<FreeNode*>(block_payload(hdr)));
}

bool contains(const void* pointer) {
    if (pointer == nullptr) {
        return false;
    }
    auto* ptr = static_cast<const uint8_t*>(pointer);
    for (size_t i = 0; i < region_count; ++i) {
        if (ptr >= regions[i].base && ptr < regions[i].limit) {
            return true;
        }
    }
    return false;
}

Stats stats() {
    Stats s = {};
    s.backing_pages = heap_backing_pages;
    s.failed_allocations = heap_failed_allocations;

    for (size_t i = 0; i < region_count; ++i) {
        auto* current = reinterpret_cast<BlockHeader*>(regions[i].base);
        auto* region_end = reinterpret_cast<BlockHeader*>(regions[i].limit);

        while (current < region_end) {
            const uint64_t bsize = block_size(current);
            if (bsize == 0) {
                break;
            }
            s.total_bytes += bsize;
            if (block_allocated(current)) {
                s.used_bytes += bsize;
            } else {
                s.free_bytes += bsize;
                ++s.free_blocks;
            }
            current = next_block(current);
        }
    }

    return s;
}

} // namespace tori::memory::heap
