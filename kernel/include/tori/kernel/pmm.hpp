#pragma once

#include <stdint.h>

#include <tori/kernel/boot_info.hpp>

namespace tori::memory::pmm {

constexpr uint64_t page_size = 4096;
constexpr uint64_t invalid_physical_address = UINT64_MAX;

struct Stats {
    uint64_t managed_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t reclaimed_bootloader_pages;
    uint64_t skipped_pages;
};

void init(const boot::BootInfo& boot_info);
uint64_t alloc_page();
uint64_t alloc_pages(uint64_t page_count);
void free_page(uint64_t physical_address);
void free_pages(uint64_t physical_address, uint64_t page_count);
bool owns_page(uint64_t physical_address);
Stats stats();

} // namespace tori::memory::pmm
