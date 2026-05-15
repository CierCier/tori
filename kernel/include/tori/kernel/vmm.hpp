#pragma once

#include <stdint.h>

namespace tori::memory::vmm {

// Page table entry flags for map_page.
enum class Flags : uint64_t {
    None          = 0,
    Present       = (1ULL << 0),
    Writable      = (1ULL << 1),
    User          = (1ULL << 2),
    WriteThrough  = (1ULL << 3),
    CacheDisable  = (1ULL << 4),
    Global        = (1ULL << 8),
    CowPending    = (1ULL << 9),
    NoExecute     = (1ULL << 63),
};

inline Flags operator|(Flags a, Flags b) {
    return static_cast<Flags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline bool operator&(Flags a, Flags b) {
    return (static_cast<uint64_t>(a) & static_cast<uint64_t>(b)) != 0;
}

// Capture the active page table (CR3) as the kernel's own.
// Must be called once during boot, before any map/unmap operations.
void init();

// The kernel's PML4 physical address (set by init).
uint64_t kernel_pml4();

// Maps a single 4KiB page in the given address space.
// pml4_phys: 0 means the kernel page table.
// If intermediate tables are missing, they are allocated via PMM.
void map_page(uint64_t virtual_address, uint64_t physical_address, Flags flags,
              uint64_t pml4_phys = 0);

// Unmaps a single 4KiB page in the given address space.
// Frees any intermediate page table pages that become empty.
// pml4_phys: 0 means the kernel page table.
void unmap_page(uint64_t virtual_address, uint64_t pml4_phys = 0);

// --- Kernel virtual address range allocator ---

// Initialize the kernel vmem range allocator.
// hhdm_top: first virtual address past the HHDM mapping.
// Free ranges are tracked from hhdm_top to the kernel image base.
void init_vmem_ranges(uint64_t hhdm_top);

// Allocate a contiguous range of kernel virtual addresses.
// Returns the base virtual address, or 0 on failure.
uint64_t alloc_vmem_range(uint64_t size);

// Free a previously allocated range (may be a no-op depending on strategy).
void free_vmem_range(uint64_t base, uint64_t size);

// Allocate a new PML4 for a userspace process.
// Kernel-space entries (indices 256-511) are cloned from the kernel PML4.
// Returns the physical address of the new PML4, or 0 on failure.
uint64_t create_user_pml4();

// Clone an entire user address space for fork.
// Walks user entries (indices 0-255) at all 4 levels, deep-copies the page table tree,
// and marks writable leaf PTEs as copy-on-write (W=0, CowPending=1).
// Flushes TLB entries for COW pages on the source PML4.
// Returns the physical address of the new PML4, or 0 on failure.
uint64_t clone_address_space(uint64_t src_pml4_phys);

// Resolve a user copy-on-write page fault for an address space.
// Returns true when the fault was handled and execution may resume.
bool handle_cow_fault(uint64_t fault_address, uint64_t error_code, uint64_t pml4_phys);

// Free all physical pages in a user address space (indices 0-255).
// Walks page tables, frees leaf physical pages and all intermediate page table pages,
// then frees the PML4 page itself.
// The caller must ensure no task is actively using this address space.
void free_address_space(uint64_t pml4_phys);

} // namespace tori::memory::vmm
