#pragma once

#include <stdint.h>

namespace tori::acpi {

struct RSDP {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct SDTHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

enum class Revision : uint8_t {
    V1 = 0,
    V2 = 2,
};

struct Info {
    bool rsdp_found;
    bool rsdp_checksum_valid;
    bool xsdt_found;
    uint32_t xsdt_entry_count;
    bool madt_found;
    bool fadt_found;
    bool hpet_found;
};

struct Signature {
    char value[4];
};

bool validate_rsdp_checksum(const RSDP* rsdp);
bool validate_sdt_checksum(const SDTHeader* header);
const SDTHeader* find_table(const RSDP* rsdp, const char signature[4]);
Info enumerate(const RSDP* rsdp);

} // namespace tori::acpi
