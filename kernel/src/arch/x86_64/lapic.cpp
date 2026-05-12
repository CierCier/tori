#include <tori/kernel/lapic.hpp>
#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/vmm.hpp>

#include "io.hpp"

namespace {

uint64_t lapic_base_virt = 0;

void write_reg(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(lapic_base_virt + offset) = value;
}

uint32_t read_reg(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(lapic_base_virt + offset);
}

// Minimal PIT delay for calibration
void pit_wait_10ms() {
    // 1193182 Hz / 100 = 11931 ticks for 10ms
    uint16_t count = 11931;
    tori::arch::x86_64::outb(0x43, 0x30); // Channel 0, lobyte/hibyte, mode 0
    tori::arch::x86_64::outb(0x40, count & 0xFF);
    tori::arch::x86_64::outb(0x40, count >> 8);

    while (true) {
        tori::arch::x86_64::outb(0x43, 0xE2); // Read back command for channel 0
        uint8_t status = tori::arch::x86_64::inb(0x40);
        if (status & 0x80) break; // Null count bit or OUT pin? 
        // Actually mode 0 OUT pin goes high when count hits 0.
        // Let's use simpler polling if possible.
    }
}

// Improved PIT poll
void pit_prepare_sleep(uint16_t ticks) {
    tori::arch::x86_64::outb(0x43, 0x34); // Channel 0, lobyte/hibyte, mode 2 (rate generator)
    tori::arch::x86_64::outb(0x40, ticks & 0xFF);
    tori::arch::x86_64::outb(0x40, ticks >> 8);
}

void pit_sleep_ticks(uint16_t ticks) {
    pit_prepare_sleep(ticks);
    uint16_t last_count = ticks;
    uint32_t elapsed = 0;
    while (elapsed < ticks) {
        tori::arch::x86_64::outb(0x43, 0x00); // Latch count
        uint8_t lo = tori::arch::x86_64::inb(0x40);
        uint8_t hi = tori::arch::x86_64::inb(0x40);
        uint16_t current_count = lo | (hi << 8);
        if (current_count < last_count) {
            elapsed += (last_count - current_count);
        } else if (current_count > last_count) {
            elapsed += (last_count + (ticks - current_count));
        }
        last_count = current_count;
    }
}

} // namespace

namespace tori::arch::x86_64::lapic {

void init(uint64_t physical_base) {
    lapic_base_virt = reinterpret_cast<uint64_t>(tori::memory::address::physical_to_virtual(physical_base));
    
    // Ensure the LAPIC range is mapped in the virtual address space.
    // Use Present | Writable | CacheDisable | WriteThrough for MMIO.
    tori::memory::vmm::map_page(lapic_base_virt, physical_base,
        tori::memory::vmm::Flags::Present |
        tori::memory::vmm::Flags::Writable |
        tori::memory::vmm::Flags::CacheDisable |
        tori::memory::vmm::Flags::WriteThrough);

    // Ensure global enable bit in IA32_APIC_BASE MSR
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
    lo |= (1 << 11);
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1B));

    // Enable LAPIC via SVR and set spurious vector to 255
    write_reg(reg_svr, read_reg(reg_svr) | 0x1FF);

    TORI_LOG_INFO("lapic", "Local APIC initialized");
    TORI_LOG_VALUE(tori::log::Level::Info, "lapic", "base virtual", lapic_base_virt);
    TORI_LOG_VALUE(tori::log::Level::Info, "lapic", "id", id());
    TORI_LOG_VALUE(tori::log::Level::Info, "lapic", "version", version());
}

void eoi() {
    write_reg(reg_eoi, 0);
}

uint32_t id() {
    return read_reg(reg_id) >> 24;
}

uint32_t version() {
    return read_reg(reg_version) & 0xFF;
}

void init_timer(uint32_t frequency_hz) {
    if (frequency_hz == 0) return;

    // 1. Tell LAPIC timer to use divisor 16
    write_reg(reg_timer_divider, 0x3);

    // 2. Calibrate using PIT (wait 10ms)
    write_reg(reg_timer_initial, 0xFFFFFFFF);
    pit_sleep_ticks(11932); // ~10ms
    uint32_t ticks_per_10ms = 0xFFFFFFFF - read_reg(reg_timer_current);
    write_reg(reg_timer_initial, 0); // Stop timer

    // 3. Calculate ticks for desired frequency
    uint32_t ticks_per_s = ticks_per_10ms * 100;
    uint32_t ticks_per_period = ticks_per_s / frequency_hz;

    // 4. Set periodic mode, vector 32
    write_reg(reg_lvt_timer, 32 | (1 << 17));
    write_reg(reg_timer_divider, 0x3);
    write_reg(reg_timer_initial, ticks_per_period);

    TORI_LOG_INFO("lapic", "Timer initialized");
    TORI_LOG_VALUE(tori::log::Level::Info, "lapic", "ticks per 10ms", ticks_per_10ms);
    TORI_LOG_VALUE(tori::log::Level::Info, "lapic", "ticks per period", ticks_per_period);
}

} // namespace tori::arch::x86_64::lapic
