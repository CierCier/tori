#include <tori/kernel/time.hpp>
#include <tori/kernel/sync/atomic.hpp>

namespace {

uint32_t designated_bsp_id = 0;
uint32_t timer_frequency_hz = 0;
tori::sync::Atomic<uint64_t> global_tick_count{0};

} // namespace

namespace tori::time {

void init(uint32_t bsp_lapic_id, uint32_t frequency_hz) {
    designated_bsp_id = bsp_lapic_id;
    timer_frequency_hz = frequency_hz;
    global_tick_count.store(0, tori::sync::MemoryOrder::relaxed);
}

void tick(uint32_t current_lapic_id) {
    if (current_lapic_id == designated_bsp_id) {
        global_tick_count.fetch_add(1, tori::sync::MemoryOrder::relaxed);
    }
}

uint64_t uptime_ms() {
    if (timer_frequency_hz == 0) return 0;

    uint64_t current_ticks = global_tick_count.load(tori::sync::MemoryOrder::relaxed);
    return (current_ticks * 1000) / timer_frequency_hz;
}

} // namespace tori::time
