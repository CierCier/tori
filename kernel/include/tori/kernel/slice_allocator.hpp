#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tori::memory::slice {

struct Stats {
    uint64_t backing_pages;
    uint64_t total_slices;
    uint64_t free_slices;
    uint64_t failed_allocations;
};

void init(uint64_t hhdm_offset);
void* alloc(size_t size, size_t alignment = alignof(uint64_t));
void free(void* pointer, size_t size);
Stats stats();

} // namespace tori::memory::slice
