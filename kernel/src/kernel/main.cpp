#include <tori/kernel/kernel.hpp>

#include <tori/kernel/acpi.hpp>
#include <tori/kernel/allocator.hpp>
#include <tori/kernel/gdt.hpp>
#include <tori/kernel/heap.hpp>
#include <tori/kernel/idt.hpp>
#include <tori/kernel/lapic.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/madt.hpp>
#include <tori/kernel/memory_map.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/slice_allocator.hpp>
#include <tori/kernel/task.hpp>
#include <tori/kernel/time.hpp>
#include <tori/kernel/timer.hpp>
#include <tori/kernel/vfs.hpp>
#include <tori/kernel/fs/ramfs.hpp>
#include <tori/kernel/fs/overlayfs.hpp>
#include <tori/kernel/vmem_layout.hpp>
#include <tori/kernel/vmm.hpp>

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
}

void log_madt_info(const tori::acpi::madt::Info& info) {
    TORI_LOG_VALUE(tori::log::Level::Info, "madt", "local apic base", info.local_apic_address);
    TORI_LOG_VALUE(tori::log::Level::Info, "madt", "usable CPUs", info.local_apic_count);
    TORI_LOG_VALUE(tori::log::Level::Info, "madt", "IOAPIC count", info.io_apic_count);
    TORI_LOG_VALUE(tori::log::Level::Info, "madt", "interrupt overrides", info.interrupt_override_count);
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

    if (info.madt_found) {
        const tori::acpi::SDTHeader* madt_header = tori::acpi::find_table(rsdp, "APIC");
        if (madt_header != nullptr) {
            const tori::acpi::madt::Info madt_info = tori::acpi::madt::parse(madt_header);
            log_madt_info(madt_info);
        }
    }

    // ACPI reclaimable memory stays reserved for now: we have not copied
    // any table data into kernel-owned storage, so firmware tables must
    // remain accessible. Release will be safe once table data is copied
    // or protected by proper VMM page ownership.
}

void log_vmem_layout() {
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "kernel image start", tori::memory::vmem::kernel_image_start());
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "kernel image size", tori::memory::vmem::kernel_image_size());
    TORI_LOG_VALUE(tori::log::Level::Info, "vmem", "kernel image end", tori::memory::vmem::kernel_image_end());
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

    return {
        .regions = regions,
        .region_count = source.region_count,
        .source_context = nullptr,
        .source_region_at = nullptr,
    };
}

struct ApData {
    size_t cpu_index;
    uint64_t lapic_base;
};

void ap_main(void* arg) {
    auto* data = static_cast<ApData*>(arg);
    size_t cpu_index = data->cpu_index;
    uint64_t lapic_base = data->lapic_base;

    tori::arch::x86_64::init_gdt();
    tori::arch::x86_64::load_tss(cpu_index);
    tori::arch::x86_64::init_idt();
    tori::arch::x86_64::lapic::init(lapic_base);
    tori::log::init_per_cpu(tori::arch::x86_64::lapic::id());
    tori::arch::x86_64::lapic::init_timer(1000);

    TORI_LOG_INFO("kernel", "AP initialized and entering idle loop");
    TORI_LOG_VALUE(tori::log::Level::Info, "kernel", "ap cpu index", cpu_index);

    for (;;) {
        asm volatile("sti; hlt" : : : "memory");
    }
}

} // namespace

namespace tori {

void kernel_main_task(void*);

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
    memory::slice::init(owned_boot_info.hhdm_offset);
    memory::heap::init();
    memory::vmm::init();

    owned_boot_info.memory_map = copy_memory_map(owned_boot_info.memory_map);
    log_vmem_layout();

    {
        uint64_t max_phys = 0;
        for (size_t i = 0; i < owned_boot_info.memory_map.region_count; ++i) {
            const auto region = boot::memory_region_at(owned_boot_info.memory_map, i);
            const uint64_t end = region.base + region.length;
            if (end > max_phys) max_phys = end;
        }
        const uint64_t hhdm_top = owned_boot_info.hhdm_offset + max_phys;
        memory::vmm::init_vmem_ranges(hhdm_top);
    }

    init_acpi(owned_boot_info);

    const tori::acpi::SDTHeader* madt_header = tori::acpi::find_table(static_cast<const tori::acpi::RSDP*>(owned_boot_info.rsdp), "APIC");
    tori::acpi::madt::Info madt_info = {};
    if (madt_header != nullptr) {
        madt_info = tori::acpi::madt::parse(madt_header);
    }

    // All ACPI table data has been copied into kernel-owned memory.
    // Release firmware ACPI reclaimable pages back to the PMM.
    tori::memory::pmm::free_acpi_reclaimable(owned_boot_info.memory_map);
    owned_boot_info.rsdp = nullptr;
    owned_boot_info.has_rsdp = false;

    // BSP Initialization
    tori::arch::x86_64::init_gdt();
    // We need to find the BSP's index in the CPU list.
    // For now, let's assume the BSP is the first CPU in the list that matches bsp_lapic_id.
    size_t bsp_index = 0;
    if (owned_boot_info.has_smp) {
        for (size_t i = 0; i < owned_boot_info.smp.cpu_count; ++i) {
            if (owned_boot_info.smp.cpus[i].lapic_id == owned_boot_info.smp.bsp_lapic_id) {
                bsp_index = i;
                break;
            }
        }
    }
    tori::time::init(owned_boot_info.smp.bsp_lapic_id, 1000);

    tori::arch::x86_64::load_tss(bsp_index);
    tori::arch::x86_64::init_idt();
    tori::arch::x86_64::init_pic();

    if (madt_header != nullptr) {
        tori::arch::x86_64::lapic::init(madt_info.local_apic_address);
        tori::log::init_per_cpu(owned_boot_info.smp.bsp_lapic_id);
        tori::arch::x86_64::lapic::init_timer(1000);
        tori::timer::init();
        tori::log::init_timer_flush();
    }

    tori::vfs::init();

    tori::vfs::Vnode* lower_root = nullptr;
    {
        int err = tori::vfs::mount(&tori::ramfs::fs_ops, nullptr, &lower_root);
        if (err < 0) {
            TORI_PANIC("vfs", "failed to mount root filesystem");
        }
        TORI_LOG_INFO("vfs", "root filesystem mounted (RamFS)");

        for (uint64_t i = 0; i < owned_boot_info.module_count; ++i) {
            auto& mod = owned_boot_info.modules[i];
            if (mod.size == 0) continue;

            const char* modpath = mod.path;
            while (*modpath == '/') ++modpath;

            const char* filename = modpath;
            for (const char* s = modpath; *s; ++s) {
                if (*s == '/') filename = s + 1;
            }

            auto* src = reinterpret_cast<const uint8_t*>(mod.address);

            int fd = 0;
            err = tori::vfs::open(lower_root, filename, tori::vfs::O_CREAT | tori::vfs::O_WRONLY, &fd);
            if (err < 0) {
                TORI_LOG_WARN("vfs", "failed to create module file");
                continue;
            }

            size_t written = 0;
            err = tori::vfs::write(fd, src, static_cast<size_t>(mod.size), &written);
            if (err < 0) {
                TORI_LOG_WARN("vfs", "failed to write module data");
            }
            tori::vfs::close(fd);

            TORI_LOG_VALUE(tori::log::Level::Info, "vfs", "loaded module",
                          static_cast<uint64_t>(mod.size));
        }
    }

    {
        // Create writable upper layer for overlayFS
        tori::vfs::Vnode* upper_root = nullptr;
        int err = tori::ramfs::fs_ops.mount(&upper_root);
        if (err < 0 || !upper_root) {
            TORI_PANIC("vfs", "failed to create upper RamFS for overlay");
        }

        tori::overlayfs::Layer layers[2];
        layers[0].root = lower_root;
        layers[0].writable = false;
        layers[1].root = upper_root;
        layers[1].writable = true;

        err = tori::overlayfs::init(layers, 2);
        if (err < 0) {
            TORI_PANIC("vfs", "failed to init overlayFS");
        }

        tori::vfs::Vnode* overlay_root = nullptr;
        err = tori::overlayfs::fs_ops.mount(&overlay_root);
        if (err < 0 || !overlay_root) {
            TORI_PANIC("vfs", "failed to mount overlayFS root");
        }

        err = tori::vfs::set_root(overlay_root);
        if (err < 0) {
            TORI_PANIC("vfs", "failed to set overlayFS as root");
        }

        TORI_LOG_INFO("vfs", "overlayFS root active (lower=RamFS, upper=RamFS)");
    }

    tori::sched::init_task_system(owned_boot_info.smp.bsp_lapic_id);

    // AP Initialization
    if (owned_boot_info.has_smp) {
        // We need a place for ApData that persists.
        static ApData ap_data[CONFIG_MAX_CPUS];

        for (size_t i = 0; i < owned_boot_info.smp.cpu_count; ++i) {
            if (i == bsp_index) continue;

            ap_data[i].cpu_index = i;
            ap_data[i].lapic_base = madt_info.local_apic_address;

            TORI_LOG_INFO("kernel", "waking up AP");
            TORI_LOG_VALUE(tori::log::Level::Info, "kernel", "ap cpu index", i);
            TORI_LOG_VALUE(tori::log::Level::Info, "kernel", "ap lapic id", owned_boot_info.smp.cpus[i].lapic_id);

            owned_boot_info.smp.wake_up_ap(owned_boot_info.smp.cpus[i], ap_main, &ap_data[i]);
        }
    }

    TORI_LOG_INFO("kernel", "boot, memory, ACPI, and CPU runtime initialized; interrupts enabled");
    TORI_LOG_INFO("kernel", "starting scheduler");

    tori::sched::Task* main_task = tori::sched::create_task(kernel_main_task, nullptr, "kernel-main");
    if (main_task == nullptr) {
        TORI_PANIC("kernel", "failed to create kernel main task");
    }

    TORI_LOG_VALUE(tori::log::Level::Info, "kernel", "main task id", main_task->id);
    tori::sched::start_scheduler();
}

void kernel_main_task(void*) {
    TORI_LOG_INFO("kernel", "first scheduled task running");
    TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "kernel", "task name", tori::sched::current_task()->name);

    {
        int fd = 0;
        int err = tori::vfs::open(nullptr, "/hello.txt", tori::vfs::O_RDONLY, &fd);
        if (err < 0) {
            TORI_LOG_WARN("vfs", "could not open /hello.txt through overlay");
        } else {
            TORI_LOG_INFO("vfs", "opened /hello.txt through overlay");

            tori::vfs::Stat st;
            err = tori::vfs::stat(fd, &st);
            if (err == 0) {
                TORI_LOG_VALUE(tori::log::Level::Info, "vfs", "hello.txt size", st.size);
            }

            char buf[128];
            size_t got = 0;
            err = tori::vfs::read(fd, buf, sizeof(buf) - 1, &got);
            if (err == 0) {
                buf[got] = '\0';
                TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "vfs", "hello.txt content", buf);
            }

            tori::vfs::close(fd);
        }
    }

    TORI_LOG_INFO("kernel", "kernel-main yielding forever");

    for (;;) {
        tori::sched::yield();
    }
}

} // namespace tori
