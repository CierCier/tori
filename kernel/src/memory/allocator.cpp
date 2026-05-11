#include <tori/kernel/allocator.hpp>

#include <stdint.h>

#include <tori/kernel/address.hpp>
#include <tori/kernel/heap.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/slice_allocator.hpp>

namespace {

uint64_t page_count_for_size(size_t size) {
    return (static_cast<uint64_t>(size) + tori::memory::pmm::page_size - 1) / tori::memory::pmm::page_size;
}

constexpr size_t heap_threshold = 1024 * 1024;

} // namespace

namespace tori::memory {

void* kalloc(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }

    if (size <= 2048 && alignment <= 16) {
        return slice::alloc(size, alignment);
    }

    if (size <= heap_threshold) {
        return heap::alloc(size, alignment);
    }

    if (alignment > pmm::page_size) {
        return nullptr;
    }

    const uint64_t page_count = page_count_for_size(size);
    const uint64_t physical = pmm::alloc_pages(page_count);
    if (physical == pmm::invalid_physical_address) {
        return nullptr;
    }

    return address::physical_to_virtual(physical);
}

void kfree(void* pointer, size_t size) {
    if (pointer == nullptr || size == 0) {
        return;
    }

    if (size <= 2048) {
        slice::free(pointer, size);
        return;
    }

    if (heap::contains(pointer)) {
        heap::free(pointer);
        return;
    }

    const uint64_t physical = address::virtual_to_physical(pointer);
    pmm::free_pages(physical, page_count_for_size(size));
}

} // namespace tori::memory
