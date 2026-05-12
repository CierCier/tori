#pragma once

#include <stdint.h>

namespace tori::arch::x86_64::lapic {

// LAPIC Register Offsets
constexpr uint32_t reg_id            = 0x020;
constexpr uint32_t reg_version       = 0x030;
constexpr uint32_t reg_tpr           = 0x080;
constexpr uint32_t reg_eoi           = 0x0B0;
constexpr uint32_t reg_ldr           = 0x0D0;
constexpr uint32_t reg_dfr           = 0x0E0;
constexpr uint32_t reg_svr           = 0x0F0;
constexpr uint32_t reg_esr           = 0x280;
constexpr uint32_t reg_icr_low       = 0x300;
constexpr uint32_t reg_icr_high      = 0x310;
constexpr uint32_t reg_lvt_timer     = 0x320;
constexpr uint32_t reg_lvt_thermal   = 0x330;
constexpr uint32_t reg_lvt_perf      = 0x340;
constexpr uint32_t reg_lvt_lint0     = 0x350;
constexpr uint32_t reg_lvt_lint1     = 0x360;
constexpr uint32_t reg_lvt_error     = 0x370;
constexpr uint32_t reg_timer_initial = 0x380;
constexpr uint32_t reg_timer_current = 0x390;
constexpr uint32_t reg_timer_divider = 0x3E0;

void init(uint64_t physical_base);
void eoi();
void init_timer(uint32_t frequency_hz);

uint32_t id();
uint32_t version();

} // namespace tori::arch::x86_64::lapic
