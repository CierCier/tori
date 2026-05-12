#include <tori/kernel/boot_info.hpp>
#include <tori/kernel/kernel.hpp>
#include <config.h>

#include <limine.h>

namespace {

__attribute__((used, section(".limine_requests")))
volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start")))
volatile uint64_t limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
volatile limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr,
};

__attribute__((used, section(".limine_requests")))
volatile limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .flags = 0,
};

struct ApStartData {
    void (*entry)(void*);
    void* arg;
};

// Static storage for CpuInfo array passed to generic kernel
tori::boot::CpuInfo generic_cpus[CONFIG_MAX_CPUS];

extern "C" void limine_ap_trampoline(struct limine_mp_info* info) {
    auto* data = reinterpret_cast<ApStartData*>(info->extra_argument);
    data->entry(data->arg);
}

void limine_wake_up_ap(const tori::boot::CpuInfo& cpu, void (*entry)(void*), void* arg) {
    auto* info = static_cast<struct limine_mp_info*>(cpu.internal_handle);
    
    // We need a place to store ApStartData that persists until the AP wakes up.
    // Since we are waking up APs one by one in a synchronous loop in kernel_main,
    // we can arguably use a single static struct, but per-CPU is safer.
    static ApStartData start_data[CONFIG_MAX_CPUS];
    uint32_t id = cpu.processor_id;
    if (id >= CONFIG_MAX_CPUS) return;

    start_data[id].entry = entry;
    start_data[id].arg = arg;

    info->extra_argument = reinterpret_cast<uint64_t>(&start_data[id]);
    info->goto_address = limine_ap_trampoline;
}

tori::boot::MemoryKind convert_memory_kind(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
        return tori::boot::MemoryKind::Usable;
    case LIMINE_MEMMAP_RESERVED:
        return tori::boot::MemoryKind::Reserved;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        return tori::boot::MemoryKind::AcpiReclaimable;
    case LIMINE_MEMMAP_ACPI_NVS:
        return tori::boot::MemoryKind::AcpiNvs;
    case LIMINE_MEMMAP_BAD_MEMORY:
        return tori::boot::MemoryKind::BadMemory;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        return tori::boot::MemoryKind::BootloaderReclaimable;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        return tori::boot::MemoryKind::KernelAndModules;
    case LIMINE_MEMMAP_FRAMEBUFFER:
        return tori::boot::MemoryKind::Framebuffer;
    case LIMINE_MEMMAP_RESERVED_MAPPED:
        return tori::boot::MemoryKind::ReservedMapped;
    default:
        return tori::boot::MemoryKind::Unknown;
    }
}

tori::boot::MemoryRegion limine_memory_region_at(const void* context, size_t index) {
    const auto* response = static_cast<const limine_memmap_response*>(context);
    if (response == nullptr || response->entries == nullptr || index >= response->entry_count) {
        return {};
    }

    const limine_memmap_entry* entry = response->entries[index];
    if (entry == nullptr) {
        return {
            .base = 0,
            .length = 0,
            .kind = tori::boot::MemoryKind::Unknown,
        };
    }

    return {
        .base = entry->base,
        .length = entry->length,
        .kind = convert_memory_kind(entry->type),
    };
}

tori::boot::MemoryMap collect_memory_map() {
    auto* response = memmap_request.response;
    if (response == nullptr || response->entries == nullptr) {
        return {
            .regions = nullptr,
            .region_count = 0,
            .source_context = nullptr,
            .source_region_at = nullptr,
        };
    }

    return {
        .regions = nullptr,
        .region_count = static_cast<size_t>(response->entry_count),
        .source_context = response,
        .source_region_at = limine_memory_region_at,
    };
}

tori::boot::Framebuffer collect_framebuffer(bool& has_framebuffer) {
    has_framebuffer = false;

    auto* response = framebuffer_request.response;
    if (response == nullptr || response->framebuffer_count == 0 || response->framebuffers == nullptr) {
        return {};
    }

    const limine_framebuffer* framebuffer = response->framebuffers[0];
    if (framebuffer == nullptr) {
        return {};
    }

    has_framebuffer = true;

    return {
        .address = framebuffer->address,
        .width = framebuffer->width,
        .height = framebuffer->height,
        .pitch = framebuffer->pitch,
        .bits_per_pixel = framebuffer->bpp,
        .red_mask_size = framebuffer->red_mask_size,
        .red_mask_shift = framebuffer->red_mask_shift,
        .green_mask_size = framebuffer->green_mask_size,
        .green_mask_shift = framebuffer->green_mask_shift,
        .blue_mask_size = framebuffer->blue_mask_size,
        .blue_mask_shift = framebuffer->blue_mask_shift,
        .format = framebuffer->memory_model == LIMINE_FRAMEBUFFER_RGB
            ? tori::boot::PixelFormat::Rgb
            : tori::boot::PixelFormat::Unknown,
    };
}

tori::boot::SmpInfo collect_smp_info(bool& has_smp) {
    has_smp = false;
    auto* response = mp_request.response;
    if (response == nullptr) {
        return {};
    }

    has_smp = true;
    uint64_t count = response->cpu_count;
    if (count > CONFIG_MAX_CPUS) {
        count = CONFIG_MAX_CPUS;
    }

    for (uint64_t i = 0; i < count; ++i) {
        auto* info = response->cpus[i];
        generic_cpus[i].processor_id = info->processor_id;
        generic_cpus[i].lapic_id = info->lapic_id;
        generic_cpus[i].internal_handle = info;
    }

    return {
        .bsp_lapic_id = response->bsp_lapic_id,
        .cpu_count = count,
        .cpus = generic_cpus,
        .wake_up_ap = limine_wake_up_ap,
    };
}

tori::boot::BootInfo collect_boot_info() {
    bool has_framebuffer = false;
    const tori::boot::Framebuffer framebuffer = collect_framebuffer(has_framebuffer);

    bool has_smp = false;
    const tori::boot::SmpInfo smp = collect_smp_info(has_smp);

    auto* bootloader_response = bootloader_info_request.response;
    auto* cmdline_response = cmdline_request.response;
    auto* hhdm_response = hhdm_request.response;
    auto* executable_address_response = executable_address_request.response;
    auto* rsdp_response = rsdp_request.response;
    auto* module_response = module_request.response;

    return {
        .source = tori::boot::BootSource::Limine,
        .bootloader_name = bootloader_response != nullptr ? bootloader_response->name : nullptr,
        .bootloader_version = bootloader_response != nullptr ? bootloader_response->version : nullptr,
        .command_line = cmdline_response != nullptr ? cmdline_response->cmdline : nullptr,
        .kernel_address = {
            .physical_base = executable_address_response != nullptr ? executable_address_response->physical_base : 0,
            .virtual_base = executable_address_response != nullptr ? executable_address_response->virtual_base : 0,
        },
        .hhdm_offset = hhdm_response != nullptr ? hhdm_response->offset : 0,
        .memory_map = collect_memory_map(),
        .framebuffer = framebuffer,
        .rsdp = rsdp_response != nullptr ? rsdp_response->address : nullptr,
        .module_count = module_response != nullptr ? module_response->module_count : 0,
        .smp = smp,
        .has_framebuffer = has_framebuffer,
        .has_hhdm = hhdm_response != nullptr,
        .has_rsdp = rsdp_response != nullptr && rsdp_response->address != nullptr,
        .has_smp = has_smp,
    };
}

} // namespace

extern "C" [[noreturn]] void limine_entry() {
    const tori::boot::BootInfo boot_info = collect_boot_info();
    tori::kernel_main(boot_info);
}
