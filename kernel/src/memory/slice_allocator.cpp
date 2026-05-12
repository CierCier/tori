#include <tori/kernel/slice_allocator.hpp>

#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/sync/spinlock.hpp>

namespace {

struct FreeSlice {
    FreeSlice* next;
};

struct SizeClass {
    uint64_t size;
    uint64_t alignment;
    FreeSlice* free_list;
    uint64_t total_slices;
    uint64_t free_slices;
    uint64_t backing_pages;
};

SizeClass classes[] = {
    {.size = 16, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 32, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 64, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 128, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 256, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 512, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 1024, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
    {.size = 2048, .alignment = 16, .free_list = nullptr, .total_slices = 0, .free_slices = 0, .backing_pages = 0},
};

tori::memory::slice::Stats allocator_stats = {};

tori::sync::Spinlock slice_lock;

bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

SizeClass* class_for(size_t size, size_t alignment) {
    if (size == 0 || !is_power_of_two(alignment)) {
        return nullptr;
    }

    const size_t required = size > sizeof(FreeSlice) ? size : sizeof(FreeSlice);

    for (SizeClass& size_class : classes) {
        if (required <= size_class.size && alignment <= size_class.alignment) {
            return &size_class;
        }
    }

    return nullptr;
}

bool grow_class(SizeClass& size_class) {
    const uint64_t physical_page = tori::memory::pmm::alloc_page();
    if (physical_page == tori::memory::pmm::invalid_physical_address) {
        ++allocator_stats.failed_allocations;
        return false;
    }

    auto* page = static_cast<uint8_t*>(tori::memory::address::physical_to_virtual(physical_page));
    const uint64_t slice_count = tori::memory::pmm::page_size / size_class.size;

    for (uint64_t index = 0; index < slice_count; ++index) {
        auto* node = reinterpret_cast<FreeSlice*>(page + index * size_class.size);
        node->next = size_class.free_list;
        size_class.free_list = node;
    }

    ++size_class.backing_pages;
    size_class.total_slices += slice_count;
    size_class.free_slices += slice_count;

    ++allocator_stats.backing_pages;
    allocator_stats.total_slices += slice_count;
    allocator_stats.free_slices += slice_count;
    return true;
}

} // namespace

namespace tori::memory::slice {

void init(uint64_t hhdm_offset) {
    address::init_hhdm(hhdm_offset);
    allocator_stats = {};

    for (SizeClass& size_class : classes) {
        size_class.free_list = nullptr;
        size_class.total_slices = 0;
        size_class.free_slices = 0;
        size_class.backing_pages = 0;
    }

    TORI_LOG_INFO("slice", "slice allocator initialized");
    TORI_LOG_VALUE(log::Level::Info, "slice", "hhdm offset", address::hhdm_offset());
}

void* alloc(size_t size, size_t alignment) {
    tori::sync::LockGuard guard(slice_lock);
    SizeClass* size_class = class_for(size, alignment);
    if (size_class == nullptr) {
        ++allocator_stats.failed_allocations;
        return nullptr;
    }

    if (size_class->free_list == nullptr && !grow_class(*size_class)) {
        return nullptr;
    }

    FreeSlice* node = size_class->free_list;
    size_class->free_list = node->next;
    --size_class->free_slices;
    --allocator_stats.free_slices;
    return node;
}

void free(void* pointer, size_t size) {
    if (pointer == nullptr) {
        return;
    }

    tori::sync::LockGuard guard(slice_lock);

    SizeClass* size_class = class_for(size, alignof(uint64_t));
    if (size_class == nullptr) {
        return;
    }

    auto* node = static_cast<FreeSlice*>(pointer);
    node->next = size_class->free_list;
    size_class->free_list = node;
    ++size_class->free_slices;
    ++allocator_stats.free_slices;
}

Stats stats() {
    tori::sync::LockGuard guard(slice_lock);
    return allocator_stats;
}

} // namespace tori::memory::slice
