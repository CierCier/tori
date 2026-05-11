#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tori::memory::heap {

struct Stats {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint64_t free_blocks;
    uint64_t backing_pages;
    uint64_t failed_allocations;
};

void init();
void* alloc(size_t size, size_t alignment = alignof(uint64_t));
void free(void* pointer);
bool contains(const void* pointer);
Stats stats();

} // namespace tori::memory::heap
