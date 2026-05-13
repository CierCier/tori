#include <tori/kernel/vmm.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/address.hpp>
#include <tori/kernel/vmem_layout.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/sync/spinlock.hpp>

namespace {

struct PageTable {
    uint64_t entries[512];
};

PageTable* get_table(uint64_t physical_address) {
    return static_cast<PageTable*>(tori::memory::address::physical_to_virtual(physical_address));
}

constexpr uint64_t pte_present = 1ULL << 0;
constexpr uint64_t pte_writable = 1ULL << 1;
constexpr uint64_t pte_user = 1ULL << 2;
constexpr uint64_t pte_huge = 1ULL << 7;
constexpr uint64_t pte_addr_mask = ~0xFFFULL;

tori::sync::Spinlock vmm_lock;

// Captured kernel PML4 physical address.
uint64_t kernel_pml4_phys = 0;

uint64_t resolve_pml4(uint64_t pml4) {
    return pml4 != 0 ? pml4 : kernel_pml4_phys;
}

void flush_tlb() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3) : : "memory");
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint64_t ensure_table(uint64_t& entry) {
    if (!(entry & pte_present)) {
        const uint64_t new_table_phys = tori::memory::pmm::alloc_page();
        if (new_table_phys == tori::memory::pmm::invalid_physical_address) {
            TORI_PANIC("vmm", "failed to allocate page table");
        }

        PageTable* table = get_table(new_table_phys);
        for (int i = 0; i < 512; ++i) {
            table->entries[i] = 0;
        }

        entry = new_table_phys | pte_present | pte_writable | pte_user;
    }
    return entry & pte_addr_mask;
}

// Split a huge page entry (level 1 = 1G, level 2 = 2M) into 512 sub-entries.
// Updates entry to point to the new intermediate table.
void split_huge(uint64_t& entry, int level) {
    const uint64_t base_aligned = entry & ~((1ULL << (level == 1 ? 30 : 21)) - 1);
    const uint64_t flags = entry & (pte_present | pte_writable | pte_user | 0x18 | (1ULL << 63));
    const uint64_t sub_size = (level == 1) ? (1ULL << 21) : (1ULL << 12);
    const bool sub_is_huge = (level == 1);

    const uint64_t new_table_phys = tori::memory::pmm::alloc_page();
    if (new_table_phys == tori::memory::pmm::invalid_physical_address) {
        TORI_PANIC("vmm", "failed to allocate page table for huge page split");
    }

    PageTable* table = get_table(new_table_phys);
    for (int i = 0; i < 512; ++i) {
        table->entries[i] = (base_aligned + i * sub_size) | flags;
        if (sub_is_huge) {
            table->entries[i] |= pte_huge;
        }
    }

    entry = new_table_phys | pte_present | pte_writable | pte_user;
    flush_tlb();
}

bool table_is_empty(PageTable* table) {
    for (int i = 0; i < 512; ++i) {
        if (table->entries[i] & pte_present) return false;
    }
    return true;
}

// Free page table pages recursively upward after an unmap.
// Returns true if the table at the given level is now empty.
bool cleanup_table_level(uint64_t phys, int level) {
    if (level == 0) return false; // never free PML4

    PageTable* table = get_table(phys);
    if (!table_is_empty(table)) return false;

    tori::memory::pmm::free_page(phys);
    return true;
}

} // namespace

namespace tori::memory::vmm {

void init() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4_phys = cr3 & pte_addr_mask;
    TORI_LOG_INFO("vmm", "VMM initialized, kernel PML4 captured");
    TORI_LOG_VALUE(log::Level::Info, "vmm", "pml4 phys",
                   static_cast<uint64_t>(kernel_pml4_phys));
}

uint64_t kernel_pml4() {
    return kernel_pml4_phys;
}

void map_page(uint64_t virtual_address, uint64_t physical_address, Flags flags,
              uint64_t pml4_phys) {
    tori::sync::LockGuard guard(vmm_lock);

    const uint64_t root = resolve_pml4(pml4_phys);
    if (!root) TORI_PANIC("vmm", "no kernel PML4 (call init first)");

    const uint64_t pml4_idx = (virtual_address >> 39) & 0x1FF;
    const uint64_t pdpt_idx = (virtual_address >> 30) & 0x1FF;
    const uint64_t pd_idx   = (virtual_address >> 21) & 0x1FF;
    const uint64_t pt_idx   = (virtual_address >> 12) & 0x1FF;

    PageTable* pml4 = get_table(root);
    uint64_t pdpt_phys = ensure_table(pml4->entries[pml4_idx]);

    PageTable* pdpt = get_table(pdpt_phys);
    if ((pdpt->entries[pdpt_idx] & pte_present) && (pdpt->entries[pdpt_idx] & pte_huge)) {
        split_huge(pdpt->entries[pdpt_idx], 1);
    }
    uint64_t pd_phys = ensure_table(pdpt->entries[pdpt_idx]);

    PageTable* pd = get_table(pd_phys);
    if ((pd->entries[pd_idx] & pte_present) && (pd->entries[pd_idx] & pte_huge)) {
        split_huge(pd->entries[pd_idx], 2);
    }
    uint64_t pt_phys = ensure_table(pd->entries[pd_idx]);

    PageTable* pt = get_table(pt_phys);
    pt->entries[pt_idx] = (physical_address & pte_addr_mask) | static_cast<uint64_t>(flags);

    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

void unmap_page(uint64_t virtual_address, uint64_t pml4_phys) {
    tori::sync::LockGuard guard(vmm_lock);

    const uint64_t root = resolve_pml4(pml4_phys);
    if (!root) TORI_PANIC("vmm", "no kernel PML4 (call init first)");

    const uint64_t pml4_idx = (virtual_address >> 39) & 0x1FF;
    const uint64_t pdpt_idx = (virtual_address >> 30) & 0x1FF;
    const uint64_t pd_idx   = (virtual_address >> 21) & 0x1FF;
    const uint64_t pt_idx   = (virtual_address >> 12) & 0x1FF;

    PageTable* pml4 = get_table(root);
    if (!(pml4->entries[pml4_idx] & pte_present)) return;
    uint64_t pdpt_phys = pml4->entries[pml4_idx] & pte_addr_mask;

    PageTable* pdpt = get_table(pdpt_phys);
    if (!(pdpt->entries[pdpt_idx] & pte_present)) return;
    uint64_t pd_phys = pdpt->entries[pdpt_idx] & pte_addr_mask;

    PageTable* pd = get_table(pd_phys);
    if (!(pd->entries[pd_idx] & pte_present)) return;
    uint64_t pt_phys = pd->entries[pd_idx] & pte_addr_mask;

    PageTable* pt = get_table(pt_phys);
    if (!(pt->entries[pt_idx] & pte_present)) return;

    pt->entries[pt_idx] = 0;
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");

    // Clean up empty intermediate tables bottom-up.
    if (cleanup_table_level(pt_phys, 1)) {
        pd->entries[pd_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    }
    if (cleanup_table_level(pd_phys, 2)) {
        pdpt->entries[pdpt_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    }
    if (cleanup_table_level(pdpt_phys, 3)) {
        pml4->entries[pml4_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    }
}

// --- Kernel virtual address range allocator (bump) ---

namespace {

// vmem range state
uint64_t vmem_current = 0;
uint64_t vmem_end = 0;

} // namespace

void init_vmem_ranges(uint64_t hhdm_top) {
    // Align up to a page boundary.
    constexpr uint64_t page_mask = 0xFFFULL;
    hhdm_top = (hhdm_top + page_mask) & ~page_mask;

    const uint64_t kernel_base = tori::memory::vmem::kernel_image_start();
    if (hhdm_top >= kernel_base) {
        TORI_LOG_WARN("vmm", "HHDM top past kernel image, no vmem range space");
        vmem_current = 0;
        vmem_end = 0;
        return;
    }

    vmem_current = hhdm_top;
    vmem_end = kernel_base;

    TORI_LOG_INFO("vmm", "kernel vmem range allocator initialized");
    TORI_LOG_VALUE(log::Level::Info, "vmm", "vmem base", vmem_current);
    TORI_LOG_VALUE(log::Level::Info, "vmm", "vmem end", vmem_end);
}

uint64_t alloc_vmem_range(uint64_t size) {
    if (size == 0) return 0;
    if (vmem_current >= vmem_end) return 0;

    // Align size to page boundary.
    constexpr uint64_t page_mask = 0xFFFULL;
    size = (size + page_mask) & ~page_mask;

    if (vmem_current + size > vmem_end || vmem_current + size < vmem_current) {
        return 0; // overflow or out of space
    }

    uint64_t result = vmem_current;
    vmem_current += size;
    return result;
}

void free_vmem_range(uint64_t base, uint64_t /*size*/) {
    // Bump allocator: free is a no-op for now.
    // A future VMA tree can reclaim ranges.
    (void)base;
}

} // namespace tori::memory::vmm
