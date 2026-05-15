#include <tori/kernel/pmm.hpp>

#include <tori/kernel/log.hpp>
#include <tori/kernel/memory_map.hpp>
#include <tori/kernel/sync/spinlock.hpp>

extern "C" char __kernel_virtual_start[];
extern "C" char __kernel_virtual_end[];

namespace {

constexpr uint64_t max_managed_pages = 1024 * 1024;
constexpr uint64_t bitmap_words = max_managed_pages / 64;
uint64_t page_bitmap[bitmap_words] = {};
uint16_t page_refcounts[max_managed_pages] = {};

tori::memory::pmm::Stats allocator_stats = {};
uint64_t next_search_page = 0;

tori::sync::Spinlock pmm_lock;

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

bool page_index_valid(uint64_t page_index) {
    return page_index < max_managed_pages;
}

bool is_page_used(uint64_t page_index) {
    return (page_bitmap[page_index / 64] & (1ull << (page_index % 64))) != 0;
}

bool page_range_free(uint64_t start_page, uint64_t page_count) {
    if (page_count == 0 || start_page >= max_managed_pages || start_page + page_count < start_page) {
        return false;
    }

    if (start_page + page_count > max_managed_pages) {
        return false;
    }

    for (uint64_t page = start_page; page < start_page + page_count; ++page) {
        if (is_page_used(page)) {
            return false;
        }
    }

    return true;
}

void mark_page_used(uint64_t page_index) {
    if (!page_index_valid(page_index) || is_page_used(page_index)) {
        return;
    }

    page_bitmap[page_index / 64] |= 1ull << (page_index % 64);
    page_refcounts[page_index] = 1;
    --allocator_stats.free_pages;
    ++allocator_stats.used_pages;
}

void mark_page_free(uint64_t page_index) {
    if (!page_index_valid(page_index) || !is_page_used(page_index)) {
        return;
    }

    page_bitmap[page_index / 64] &= ~(1ull << (page_index % 64));
    page_refcounts[page_index] = 0;
    ++allocator_stats.free_pages;
    --allocator_stats.used_pages;
}

void reserve_all_pages() {
    for (uint64_t index = 0; index < bitmap_words; ++index) {
        page_bitmap[index] = UINT64_MAX;
    }

    for (uint64_t index = 0; index < max_managed_pages; ++index) {
        page_refcounts[index] = 0;
    }

    allocator_stats = {};
    allocator_stats.managed_pages = max_managed_pages;
    allocator_stats.used_pages = max_managed_pages;
    next_search_page = 0;
}

void free_range(uint64_t base, uint64_t length, bool bootloader_reclaimable) {
    if (length == 0 || base + length < base) {
        return;
    }

    const uint64_t start = align_up(base, tori::memory::pmm::page_size);
    uint64_t end = align_down(base + length, tori::memory::pmm::page_size);

    if (end <= start) {
        return;
    }

    uint64_t start_page = start / tori::memory::pmm::page_size;
    uint64_t end_page = end / tori::memory::pmm::page_size;

    if (start_page >= max_managed_pages) {
        allocator_stats.skipped_pages += end_page - start_page;
        return;
    }

    if (end_page > max_managed_pages) {
        allocator_stats.skipped_pages += end_page - max_managed_pages;
        end_page = max_managed_pages;
    }

    for (uint64_t page = start_page; page < end_page; ++page) {
        if (is_page_used(page)) {
            mark_page_free(page);
            if (bootloader_reclaimable) {
                ++allocator_stats.reclaimed_bootloader_pages;
            }
        }
    }
}

void reserve_range(uint64_t base, uint64_t length) {
    if (length == 0 || base + length < base) {
        return;
    }

    const uint64_t start = align_down(base, tori::memory::pmm::page_size);
    const uint64_t end = align_up(base + length, tori::memory::pmm::page_size);
    uint64_t start_page = start / tori::memory::pmm::page_size;
    uint64_t end_page = end / tori::memory::pmm::page_size;

    if (start_page >= max_managed_pages) {
        return;
    }

    if (end_page > max_managed_pages) {
        end_page = max_managed_pages;
    }

    for (uint64_t page = start_page; page < end_page; ++page) {
        mark_page_used(page);
    }
}

uint64_t kernel_physical_size() {
    const uint64_t start = reinterpret_cast<uint64_t>(__kernel_virtual_start);
    const uint64_t end = reinterpret_cast<uint64_t>(__kernel_virtual_end);
    return end > start ? end - start : 0;
}

} // namespace

namespace tori::memory::pmm {

void init(const boot::BootInfo& boot_info) {
    reserve_all_pages();

    for (size_t index = 0; index < boot_info.memory_map.region_count; ++index) {
        const boot::MemoryRegion region = boot::memory_region_at(boot_info.memory_map, index);
        if (!memory::is_initially_allocatable(region.kind)) {
            continue;
        }

        free_range(region.base, region.length, region.kind == boot::MemoryKind::BootloaderReclaimable);
    }

    reserve_range(0, page_size);
    reserve_range(boot_info.kernel_address.physical_base, kernel_physical_size());

    TORI_LOG_INFO("pmm", "physical page allocator initialized");
    TORI_LOG_VALUE(log::Level::Info, "pmm", "managed pages", allocator_stats.managed_pages);
    TORI_LOG_VALUE(log::Level::Info, "pmm", "free pages", allocator_stats.free_pages);
    TORI_LOG_VALUE(log::Level::Info, "pmm", "used pages", allocator_stats.used_pages);
    TORI_LOG_VALUE(log::Level::Info, "pmm", "reclaimed bootloader pages", allocator_stats.reclaimed_bootloader_pages);
    TORI_LOG_VALUE(log::Level::Info, "pmm", "skipped pages", allocator_stats.skipped_pages);
}

uint64_t alloc_page() {
    return alloc_pages(1);
}

uint64_t alloc_pages(uint64_t page_count) {
    if (page_count == 0 || page_count > max_managed_pages) {
        return invalid_physical_address;
    }

    tori::sync::LockGuard guard(pmm_lock);

    for (uint64_t offset = 0; offset < max_managed_pages; ++offset) {
        const uint64_t page = (next_search_page + offset) % max_managed_pages;
        if (page_range_free(page, page_count)) {
            for (uint64_t index = 0; index < page_count; ++index) {
                mark_page_used(page + index);
            }

            next_search_page = (page + page_count) % max_managed_pages;
            return page * page_size;
        }
    }

    return invalid_physical_address;
}

void retain_page(uint64_t physical_address) {
    if ((physical_address % page_size) != 0) {
        return;
    }

    tori::sync::LockGuard guard(pmm_lock);

    const uint64_t page = physical_address / page_size;
    if (!page_index_valid(page) || !is_page_used(page)) {
        return;
    }

    if (page_refcounts[page] != UINT16_MAX) {
        ++page_refcounts[page];
    }
}

uint64_t page_ref_count(uint64_t physical_address) {
    if ((physical_address % page_size) != 0) {
        return 0;
    }

    tori::sync::LockGuard guard(pmm_lock);

    const uint64_t page = physical_address / page_size;
    if (!page_index_valid(page) || !is_page_used(page)) {
        return 0;
    }

    return page_refcounts[page];
}

void free_page(uint64_t physical_address) {
    free_pages(physical_address, 1);
}

void free_pages(uint64_t physical_address, uint64_t page_count) {
    if ((physical_address % page_size) != 0) {
        return;
    }

    if (page_count == 0) {
        return;
    }

    tori::sync::LockGuard guard(pmm_lock);

    const uint64_t page = physical_address / page_size;
    if (!page_index_valid(page) || page + page_count < page) {
        return;
    }

    uint64_t end_page = page + page_count;
    if (end_page > max_managed_pages) {
        end_page = max_managed_pages;
    }

    for (uint64_t index = page; index < end_page; ++index) {
        if (!is_page_used(index)) {
            continue;
        }
        if (page_refcounts[index] > 1) {
            --page_refcounts[index];
            continue;
        }
        mark_page_free(index);
    }

    if (page < next_search_page) {
        next_search_page = page;
    }
}

bool owns_page(uint64_t physical_address) {
    if ((physical_address % page_size) != 0) {
        return false;
    }

    return page_index_valid(physical_address / page_size);
}

Stats stats() {
    tori::sync::LockGuard guard(pmm_lock);
    return allocator_stats;
}

void free_acpi_reclaimable(const boot::MemoryMap& memory_map) {
    tori::sync::LockGuard guard(pmm_lock);

    Stats before = allocator_stats;

    for (size_t index = 0; index < memory_map.region_count; ++index) {
        const boot::MemoryRegion region = boot::memory_region_at(memory_map, index);
        if (region.kind != boot::MemoryKind::AcpiReclaimable) continue;
        if (region.length == 0 || region.base + region.length < region.base) continue;

        const uint64_t start = align_up(region.base, page_size);
        uint64_t end = align_down(region.base + region.length, page_size);
        if (end <= start) continue;

        uint64_t start_page = start / page_size;
        uint64_t end_page = end / page_size;
        if (start_page >= max_managed_pages) continue;
        if (end_page > max_managed_pages) end_page = max_managed_pages;

        for (uint64_t page = start_page; page < end_page; ++page) {
            mark_page_free(page);
        }
    }

    uint64_t freed_pages = allocator_stats.free_pages - before.free_pages;
    TORI_LOG_VALUE(log::Level::Info, "pmm", "freed ACPI reclaimable pages", freed_pages);
}

} // namespace tori::memory::pmm
