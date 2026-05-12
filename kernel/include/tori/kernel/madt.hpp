#pragma once

#include <stddef.h>
#include <stdint.h>
#include <tori/kernel/acpi.hpp>
#include <config.h>

namespace tori::acpi::madt {

struct LocalApic {
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
    bool usable;
};

struct IoApic {
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
};

struct InterruptOverride {
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

struct Info {
    uint64_t local_apic_address;

    LocalApic local_apics[CONFIG_MAX_CPUS];
    size_t local_apic_count;

    static constexpr size_t max_io_apics = 8;
    IoApic io_apics[max_io_apics];
    size_t io_apic_count;

    static constexpr size_t max_interrupt_overrides = 16;
    InterruptOverride interrupt_overrides[max_interrupt_overrides];
    size_t interrupt_override_count;
};

Info parse(const SDTHeader* header);

} // namespace tori::acpi::madt
