#include <tori/kernel/madt.hpp>
#include <tori/kernel/log.hpp>

namespace {

struct [[gnu::packed]] MADTHeader {
    tori::acpi::SDTHeader sdt;
    uint32_t local_apic_address;
    uint32_t flags;
};

struct [[gnu::packed]] RecordHeader {
    uint8_t type;
    uint8_t length;
};

struct [[gnu::packed]] RecordProcessorLocalApic {
    RecordHeader header;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
};

struct [[gnu::packed]] RecordIoApic {
    RecordHeader header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
};

struct [[gnu::packed]] RecordInterruptSourceOverride {
    RecordHeader header;
    uint8_t bus;
    uint8_t source;
    uint32_t global_system_interrupt;
    uint16_t flags;
};

struct [[gnu::packed]] RecordLocalApicAddressOverride {
    RecordHeader header;
    uint16_t reserved;
    uint64_t local_apic_address;
};

} // namespace

namespace tori::acpi::madt {

Info parse(const SDTHeader* header) {
    Info info = {};

    if (header == nullptr) {
        return info;
    }

    auto* madt = reinterpret_cast<const MADTHeader*>(header);
    info.local_apic_address = madt->local_apic_address;

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(madt + 1);
    const uint8_t* end = reinterpret_cast<const uint8_t*>(header) + header->length;

    while (ptr < end) {
        auto* record = reinterpret_cast<const RecordHeader*>(ptr);
        if (ptr + record->length > end || record->length < 2) {
            break;
        }

        switch (record->type) {
        case 0: { // Processor Local APIC
            auto* lapic = reinterpret_cast<const RecordProcessorLocalApic*>(ptr);
            if (info.local_apic_count < CONFIG_MAX_CPUS) {
                bool enabled = (lapic->flags & 1) != 0;
                bool online_capable = (lapic->flags & 2) != 0;
                
                info.local_apics[info.local_apic_count] = {
                    .processor_id = lapic->processor_id,
                    .apic_id = lapic->apic_id,
                    .flags = lapic->flags,
                    .usable = enabled || online_capable,
                };
                info.local_apic_count++;
            }
            break;
        }
        case 1: { // I/O APIC
            auto* ioapic = reinterpret_cast<const RecordIoApic*>(ptr);
            if (info.io_apic_count < Info::max_io_apics) {
                info.io_apics[info.io_apic_count] = {
                    .id = ioapic->io_apic_id,
                    .address = ioapic->io_apic_address,
                    .gsi_base = ioapic->global_system_interrupt_base,
                };
                info.io_apic_count++;
            }
            break;
        }
        case 2: { // Interrupt Source Override
            auto* override = reinterpret_cast<const RecordInterruptSourceOverride*>(ptr);
            if (info.interrupt_override_count < Info::max_interrupt_overrides) {
                info.interrupt_overrides[info.interrupt_override_count] = {
                    .bus = override->bus,
                    .source = override->source,
                    .gsi = override->global_system_interrupt,
                    .flags = override->flags,
                };
                info.interrupt_override_count++;
            }
            break;
        }
        case 5: { // Local APIC Address Override
            auto* addr_override = reinterpret_cast<const RecordLocalApicAddressOverride*>(ptr);
            info.local_apic_address = addr_override->local_apic_address;
            break;
        }
        }

        ptr += record->length;
    }

    return info;
}

} // namespace tori::acpi::madt
