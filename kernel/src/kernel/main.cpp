#include <tori/kernel/kernel.hpp>

#include <tori/kernel/acpi.hpp>
#include <tori/kernel/allocator.hpp>
#include <tori/kernel/heap.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/memory_map.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/slice_allocator.hpp>
#include <tori/kernel/vmem_layout.hpp>

#include "../arch/x86_64/halt.hpp"

namespace {

void log_boot_info(const tori::boot::BootInfo& boot_info) {
    TORI_LOG_INFO("boot", "Tori kernel entered generic kernel_main");
    TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "boot", "bootloader", boot_info.bootloader_name);
    TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "boot", "bootloader version", boot_info.bootloader_version);
    TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "boot", "command line", boot_info.command_line);
    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "kernel physical base", boot_info.kernel_address.physical_base);
    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "kernel virtual base", boot_info.kernel_address.virtual_base);
    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "hhdm offset", boot_info.hhdm_offset);
    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "rsdp", reinterpret_cast<uint64_t>(boot_info.rsdp));
    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "module count", boot_info.module_count);

    if (boot_info.has_framebuffer) {
        TORI_LOG_VALUE(tori::log::Level::Info, "fb", "width", boot_info.framebuffer.width);
        TORI_LOG_VALUE(tori::log::Level::Info, "fb", "height", boot_info.framebuffer.height);
        TORI_LOG_VALUE(tori::log::Level::Info, "fb", "pitch", boot_info.framebuffer.pitch);
        TORI_LOG_VALUE(tori::log::Level::Info, "fb", "bpp", boot_info.framebuffer.bits_per_pixel);
    } else {
        TORI_LOG_WARN("fb", "no framebuffer was provided; serial logging only");
    }
}

void log_memory_summary(const tori::boot::MemoryMap& memory_map) {
    const tori::memory::MemorySummary summary = tori::memory::summarize(memory_map);

    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "region count", memory_map.region_count);
    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "total bytes", summary.total_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "usable bytes", summary.usable_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "bootloader reclaimable bytes", summary.reclaimable_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "reserved bytes", summary.reserved_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "framebuffer bytes", summary.framebuffer_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "mem", "invalid regions", summary.invalid_regions);

    for (size_t index = 0; index < memory_map.region_count; ++index) {
        const tori::boot::MemoryRegion region = tori::boot::memory_region_at(memory_map, index);
        TORI_LOG_TEXT_VALUE(tori::log::Level::Debug, "mem", "region kind", tori::memory::memory_kind_name(region.kind));
        TORI_LOG_VALUE(tori::log::Level::Debug, "mem", "region base", region.base);
        TORI_LOG_VALUE(tori::log::Level::Debug, "mem", "region length", region.length);
    }
}

void smoke_test_pmm() {
    const uint64_t first = tori::memory::pmm::alloc_page();
    const uint64_t second = tori::memory::pmm::alloc_page();
    const uint64_t contiguous = tori::memory::pmm::alloc_pages(3);

    if (first == tori::memory::pmm::invalid_physical_address ||
        second == tori::memory::pmm::invalid_physical_address ||
        contiguous == tori::memory::pmm::invalid_physical_address) {
        TORI_PANIC("pmm", "physical page allocator could not allocate smoke-test pages");
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "pmm", "smoke allocation 1", first);
    TORI_LOG_VALUE(tori::log::Level::Info, "pmm", "smoke allocation 2", second);
    TORI_LOG_VALUE(tori::log::Level::Info, "pmm", "smoke contiguous allocation", contiguous);

    tori::memory::pmm::free_page(first);
    const uint64_t reused = tori::memory::pmm::alloc_page();
    TORI_LOG_VALUE(tori::log::Level::Info, "pmm", "smoke reused page", reused);

    if (reused != first) {
        TORI_PANIC("pmm", "physical page allocator did not reuse a freed page");
    }

    tori::memory::pmm::free_page(reused);
    tori::memory::pmm::free_page(second);
    tori::memory::pmm::free_pages(contiguous, 3);
}

void init_acpi(const tori::boot::BootInfo& boot_info) {
    if (!boot_info.has_rsdp || boot_info.rsdp == nullptr) {
        TORI_LOG_WARN("acpi", "no RSDP provided by bootloader");
        return;
    }

    auto* rsdp = static_cast<const tori::acpi::RSDP*>(boot_info.rsdp);
    const tori::acpi::Info info = tori::acpi::enumerate(rsdp);

    if (!info.rsdp_checksum_valid) {
        TORI_LOG_WARN("acpi", "ACPI RSDP is invalid; ACPI unavailable");
        return;
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "acpi", "xsdt entry count", info.xsdt_entry_count);
    TORI_LOG_VALUE(tori::log::Level::Info, "acpi", "madt present", info.madt_found ? 1ULL : 0ULL);
    TORI_LOG_VALUE(tori::log::Level::Info, "acpi", "fadt present", info.fadt_found ? 1ULL : 0ULL);
    TORI_LOG_VALUE(tori::log::Level::Info, "acpi", "hpet present", info.hpet_found ? 1ULL : 0ULL);

    // ACPI reclaimable memory stays reserved for now: we have not copied
    // any table data into kernel-owned storage, so firmware tables must
    // remain accessible. Release will be safe once table data is copied
    // or protected by proper VMM page ownership.
}

void smoke_test_heap() {
    void* block_4k = tori::memory::heap::alloc(4096, 16);
    void* block_8k = tori::memory::heap::alloc(8192, 16);
    void* block_256 = tori::memory::heap::alloc(256, 64);

    if (block_4k == nullptr || block_8k == nullptr || block_256 == nullptr) {
        TORI_PANIC("heap", "heap allocator could not allocate smoke-test blocks");
    }

    if ((reinterpret_cast<uint64_t>(block_256) & 63) != 0) {
        TORI_PANIC("heap", "heap allocator did not respect 64-byte alignment");
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "smoke 4K block", reinterpret_cast<uint64_t>(block_4k));
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "smoke 8K block", reinterpret_cast<uint64_t>(block_8k));
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "smoke 256B aligned block", reinterpret_cast<uint64_t>(block_256));

    tori::memory::heap::free(block_4k);
    void* reused = tori::memory::heap::alloc(4096, 16);
    if (reused == nullptr) {
        TORI_PANIC("heap", "heap allocator could not reuse freed memory");
    }
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "smoke reused block", reinterpret_cast<uint64_t>(reused));

    tori::memory::heap::free(reused);
    tori::memory::heap::free(block_8k);
    tori::memory::heap::free(block_256);

    const tori::memory::heap::Stats heap_stats = tori::memory::heap::stats();
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "total bytes", heap_stats.total_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "free bytes", heap_stats.free_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "used bytes", heap_stats.used_bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "free blocks", heap_stats.free_blocks);
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "backing pages", heap_stats.backing_pages);
    TORI_LOG_VALUE(tori::log::Level::Info, "heap", "failed allocations", heap_stats.failed_allocations);
}

void log_vmem_layout() {
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "kernel image start", tori::memory::vmem::kernel_image_start());
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "kernel image size", tori::memory::vmem::kernel_image_size());
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "kernel image end", tori::memory::vmem::kernel_image_end());
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "is kernel address check", tori::memory::vmem::is_kernel_address(0xFFFFFFFF80000000ull) ? 1ULL : 0ULL);
}

void smoke_test_slice_allocator() {
    const tori::memory::pmm::Stats before_pages = tori::memory::pmm::stats();

    void* small = tori::memory::slice::alloc(24, 16);
    void* medium = tori::memory::slice::alloc(128, 16);
    void* large = tori::memory::slice::alloc(1500, 16);

    if (small == nullptr || medium == nullptr || large == nullptr) {
        TORI_PANIC("slice", "slice allocator could not allocate smoke-test blocks");
    }

    if ((reinterpret_cast<uint64_t>(small) & 0xf) != 0 ||
        (reinterpret_cast<uint64_t>(medium) & 0xf) != 0 ||
        (reinterpret_cast<uint64_t>(large) & 0xf) != 0) {
        TORI_PANIC("slice", "slice allocator returned an unaligned block");
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "smoke small block", reinterpret_cast<uint64_t>(small));
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "smoke medium block", reinterpret_cast<uint64_t>(medium));
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "smoke large block", reinterpret_cast<uint64_t>(large));

    tori::memory::slice::free(small, 24);
    void* reused = tori::memory::slice::alloc(24, 16);

    if (reused != small) {
        TORI_PANIC("slice", "slice allocator did not reuse a freed block");
    }

    tori::memory::slice::free(reused, 24);
    tori::memory::slice::free(medium, 128);
    tori::memory::slice::free(large, 1500);

    const tori::memory::slice::Stats slice_stats = tori::memory::slice::stats();
    const tori::memory::pmm::Stats after_pages = tori::memory::pmm::stats();

    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "backing pages", slice_stats.backing_pages);
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "total slices", slice_stats.total_slices);
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "free slices", slice_stats.free_slices);
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "failed allocations", slice_stats.failed_allocations);
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "pmm free pages before", before_pages.free_pages);
    TORI_LOG_VALUE(tori::log::Level::Info, "slice", "pmm free pages after", after_pages.free_pages);

    if (slice_stats.backing_pages == 0 || after_pages.free_pages >= before_pages.free_pages) {
        TORI_PANIC("slice", "slice allocator did not consume backing pages");
    }
}

tori::boot::MemoryMap copy_memory_map(const tori::boot::MemoryMap& source) {
    const size_t bytes = source.region_count * sizeof(tori::boot::MemoryRegion);
    auto* regions = static_cast<tori::boot::MemoryRegion*>(tori::memory::kalloc(bytes, alignof(tori::boot::MemoryRegion)));
    if (regions == nullptr) {
        TORI_PANIC("boot", "could not allocate owned memory map");
    }

    for (size_t index = 0; index < source.region_count; ++index) {
        regions[index] = tori::boot::memory_region_at(source, index);
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "owned memory map bytes", bytes);
    TORI_LOG_VALUE(tori::log::Level::Info, "boot", "owned memory map address", reinterpret_cast<uint64_t>(regions));

    return {
        .regions = regions,
        .region_count = source.region_count,
        .source_context = nullptr,
        .source_region_at = nullptr,
    };
}

} // namespace

namespace tori {

[[noreturn]] void kernel_main(const boot::BootInfo& boot_info) {
    boot::BootInfo owned_boot_info = boot_info;

    log::init_serial();

    if (owned_boot_info.has_framebuffer) {
        log::init_framebuffer(owned_boot_info.framebuffer);
    }

    if (owned_boot_info.memory_map.region_count == 0 ||
        (owned_boot_info.memory_map.regions == nullptr && owned_boot_info.memory_map.source_region_at == nullptr)) {
        TORI_PANIC("boot", "boot memory map is missing");
    }

    log_boot_info(owned_boot_info);
    log_memory_summary(owned_boot_info.memory_map);
    memory::pmm::init(owned_boot_info);
    smoke_test_pmm();
    memory::slice::init(owned_boot_info.hhdm_offset);
    memory::heap::init();
    owned_boot_info.memory_map = copy_memory_map(owned_boot_info.memory_map);
    smoke_test_slice_allocator();
    log_vmem_layout();
    init_acpi(owned_boot_info);
    smoke_test_heap();
    TORI_LOG_INFO("kernel", "boot, memory, and ACPI initialization complete; halting");

    arch::x86_64::halt_forever();
}

} // namespace tori
