#include <tori/kernel/acpi.hpp>

#include <stddef.h>

#include <tori/kernel/address.hpp>

namespace {

constexpr const char rsdp_signature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
constexpr size_t rsdp_v1_length = 20;

bool signature_matches(const char* actual, const char expected[4]) {
    return actual[0] == expected[0] &&
           actual[1] == expected[1] &&
           actual[2] == expected[2] &&
           actual[3] == expected[3];
}

bool rsdp_sig_valid(const tori::acpi::RSDP* rsdp) {
    for (int i = 0; i < 8; ++i) {
        if (rsdp->signature[i] != rsdp_signature[i]) {
            return false;
        }
    }
    return true;
}

uint8_t sum_bytes(const uint8_t* data, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) {
        sum += data[i];
    }
    return sum;
}

void* phys_to_virt(uint64_t physical, uint64_t hhdm_offset) {
    return reinterpret_cast<void*>(physical + hhdm_offset);
}

} // namespace

namespace tori::acpi {

bool validate_rsdp_checksum(const RSDP* rsdp) {
    if (!rsdp_sig_valid(rsdp)) {
        return false;
    }

    // ACPI v1 checksum: first 20 bytes must sum to 0
    const uint8_t v1_sum = sum_bytes(reinterpret_cast<const uint8_t*>(rsdp), rsdp_v1_length);
    if (v1_sum != 0) {
        return false;
    }

    // ACPI v2+ extended checksum: entire RSDP must sum to 0
    if (rsdp->revision >= static_cast<uint8_t>(Revision::V2)) {
        const uint32_t length = rsdp->length;
        const uint8_t ext_sum = sum_bytes(reinterpret_cast<const uint8_t*>(rsdp), length);
        return ext_sum == 0;
    }

    return true;
}

bool validate_sdt_checksum(const SDTHeader* header) {
    return sum_bytes(reinterpret_cast<const uint8_t*>(header), header->length) == 0;
}

const SDTHeader* find_table(const RSDP* rsdp, const char signature[4]) {
    if (rsdp == nullptr || !rsdp_sig_valid(rsdp)) {
        return nullptr;
    }

    const uint64_t hhdm_offset = tori::memory::address::hhdm_offset();
    const uint64_t xsdt_phys = rsdp->xsdt_address;
    const uint64_t rsdt_phys = rsdp->rsdt_address;

    if (xsdt_phys != 0 && rsdp->revision >= static_cast<uint8_t>(Revision::V2)) {
        // Prefer XSDT (64-bit entry table)
        auto* xsdt = static_cast<const SDTHeader*>(phys_to_virt(xsdt_phys, hhdm_offset));
        if (signature_matches(xsdt->signature, "XSDT") && validate_sdt_checksum(xsdt)) {
            const uint32_t entry_count = (xsdt->length - sizeof(SDTHeader)) / 8;
            for (uint32_t i = 0; i < entry_count; ++i) {
                const uint64_t* entries = reinterpret_cast<const uint64_t*>(xsdt + 1);
                const uint64_t table_phys = entries[i];
                if (table_phys == 0) {
                    continue;
                }
                auto* table = static_cast<const SDTHeader*>(phys_to_virt(table_phys, hhdm_offset));
                if (signature_matches(table->signature, signature)) {
                    return table;
                }
            }
        }
    }

    if (rsdt_phys != 0) {
        // Fallback to RSDT (32-bit entry table)
        auto* rsdt = static_cast<const SDTHeader*>(phys_to_virt(rsdt_phys, hhdm_offset));
        if (signature_matches(rsdt->signature, "RSDT") && validate_sdt_checksum(rsdt)) {
            const uint32_t entry_count = (rsdt->length - sizeof(SDTHeader)) / 4;
            for (uint32_t i = 0; i < entry_count; ++i) {
                const uint32_t* entries = reinterpret_cast<const uint32_t*>(rsdt + 1);
                const uint32_t table_phys = entries[i];
                if (table_phys == 0) {
                    continue;
                }
                auto* table = static_cast<const SDTHeader*>(phys_to_virt(table_phys, hhdm_offset));
                if (signature_matches(table->signature, signature)) {
                    return table;
                }
            }
        }
    }

    return nullptr;
}

Info enumerate(const RSDP* rsdp) {
    Info info = {};

    if (rsdp == nullptr) {
        return info;
    }

    info.rsdp_found = true;

    if (!rsdp_sig_valid(rsdp)) {
        return info;
    }

    if (!validate_rsdp_checksum(rsdp)) {
        return info;
    }

    info.rsdp_checksum_valid = true;

    // Try XSDT first (ACPI v2+)
    if (rsdp->xsdt_address != 0 && rsdp->revision >= static_cast<uint8_t>(Revision::V2)) {
        auto* xsdt = static_cast<const SDTHeader*>(phys_to_virt(rsdp->xsdt_address, tori::memory::address::hhdm_offset()));
        if (signature_matches(xsdt->signature, "XSDT") && validate_sdt_checksum(xsdt)) {
            info.xsdt_found = true;
            info.xsdt_entry_count = (xsdt->length - sizeof(SDTHeader)) / 8;

            const uint64_t* entries = reinterpret_cast<const uint64_t*>(xsdt + 1);
            for (uint32_t i = 0; i < info.xsdt_entry_count; ++i) {
                const uint64_t table_phys = entries[i];
                if (table_phys == 0) {
                    continue;
                }
                auto* table = static_cast<const SDTHeader*>(phys_to_virt(table_phys, tori::memory::address::hhdm_offset()));
                if (!validate_sdt_checksum(table)) {
                    continue;
                }

                if (signature_matches(table->signature, "APIC")) {
                    info.madt_found = true;
                } else if (signature_matches(table->signature, "FACP")) {
                    info.fadt_found = true;
                } else if (signature_matches(table->signature, "HPET")) {
                    info.hpet_found = true;
                }
            }
            return info;
        }
    }

    // Fallback to RSDT (ACPI v1)
    if (rsdp->rsdt_address != 0) {
        auto* rsdt = static_cast<const SDTHeader*>(phys_to_virt(rsdp->rsdt_address, tori::memory::address::hhdm_offset()));
        if (signature_matches(rsdt->signature, "RSDT") && validate_sdt_checksum(rsdt)) {
            const uint32_t entry_count = (rsdt->length - sizeof(SDTHeader)) / 4;

            const uint32_t* entries = reinterpret_cast<const uint32_t*>(rsdt + 1);
            for (uint32_t i = 0; i < entry_count; ++i) {
                const uint32_t table_phys = entries[i];
                if (table_phys == 0) {
                    continue;
                }
                auto* table = static_cast<const SDTHeader*>(phys_to_virt(table_phys, tori::memory::address::hhdm_offset()));
                if (!validate_sdt_checksum(table)) {
                    continue;
                }

                if (signature_matches(table->signature, "APIC")) {
                    info.madt_found = true;
                } else if (signature_matches(table->signature, "FACP")) {
                    info.fadt_found = true;
                } else if (signature_matches(table->signature, "HPET")) {
                    info.hpet_found = true;
                }
            }
        }
    }

    return info;
}

} // namespace tori::acpi
