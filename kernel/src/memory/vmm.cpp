#include <tori/kernel/vmm.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/sync/spinlock.hpp>

namespace {

struct PageTable {
    uint64_t entries[512];
};

PageTable* get_table(uint64_t physical_address) {
    return static_cast<PageTable*>(tori::memory::address::physical_to_virtual(physical_address));
}

uint64_t ensure_table(uint64_t& entry) {
    if (!(entry & 1)) {
        const uint64_t new_table_phys = tori::memory::pmm::alloc_page();
        if (new_table_phys == tori::memory::pmm::invalid_physical_address) {
            TORI_PANIC("vmm", "failed to allocate page table");
        }

        PageTable* table = get_table(new_table_phys);
        for (int i = 0; i < 512; ++i) {
            table->entries[i] = 0;
        }

        // Table is present, writable, and user-accessible (caller can restrict via final entry)
        entry = new_table_phys | 1 | 2 | 4;
    }
    return entry & ~0xFFFULL;
}

tori::sync::Spinlock vmm_lock;

} // namespace

namespace tori::memory::vmm {

void map_page(uint64_t virtual_address, uint64_t physical_address, Flags flags) {
    tori::sync::LockGuard guard(vmm_lock);
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    const uint64_t pml4_phys = cr3 & ~0xFFFULL;

    const uint64_t pml4_idx = (virtual_address >> 39) & 0x1FF;
    const uint64_t pdpt_idx = (virtual_address >> 30) & 0x1FF;
    const uint64_t pd_idx   = (virtual_address >> 21) & 0x1FF;
    const uint64_t pt_idx   = (virtual_address >> 12) & 0x1FF;

    PageTable* pml4 = get_table(pml4_phys);
    uint64_t pdpt_phys = ensure_table(pml4->entries[pml4_idx]);
    
    PageTable* pdpt = get_table(pdpt_phys);
    uint64_t pd_phys = ensure_table(pdpt->entries[pdpt_idx]);
    
    PageTable* pd = get_table(pd_phys);
    uint64_t pt_phys = ensure_table(pd->entries[pd_idx]);
    
    PageTable* pt = get_table(pt_phys);
    pt->entries[pt_idx] = (physical_address & ~0xFFFULL) | static_cast<uint64_t>(flags);

    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

} // namespace tori::memory::vmm
