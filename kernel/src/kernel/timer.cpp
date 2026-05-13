#include <tori/kernel/timer.hpp>

#include <tori/kernel/lapic.hpp>
#include <tori/kernel/time.hpp>
#include <tori/kernel/sync/irq_spinlock.hpp>
#include <config.h>

namespace {

using namespace tori::timer;

constexpr size_t kBucketCount = CONFIG_TIMER_WHEEL_BUCKETS;

struct WheelState {
    TimerBucket level0[kBucketCount];
    TimerBucket level1[kBucketCount];
    uint32_t cursor;
    uint32_t l1_cursor;
    uint64_t tick_count;
};

WheelState wheel;
TimerEntry pool[CONFIG_TIMER_MAX_ENTRIES];
TimerBucket free_entries;
bool initialized = false;
uint32_t bsp_lapic_id = 0;

tori::sync::IrqSpinlock timer_lock;

TimerEntry* alloc_entry() {
    tori::sync::IrqLockGuard guard(timer_lock);
    return free_entries.pop_front();
}

void free_entry(TimerEntry* entry) {
    entry->callback = nullptr;
    entry->user_data = nullptr;
    entry->deadline_ticks = 0;
    entry->period_ticks = 0;
    tori::sync::IrqLockGuard guard(timer_lock);
    free_entries.push_back(entry);
}

void insert_level0(TimerEntry* entry, uint64_t remaining) {
    size_t slot = (wheel.cursor + remaining) % kBucketCount;
    entry->wheel_level = 0;
    entry->wheel_slot = static_cast<uint32_t>(slot);
    wheel.level0[slot].push_back(entry);
}

void insert_level1(TimerEntry* entry, uint64_t remaining) {
    size_t slot = (remaining / kBucketCount) - 1;
    if (slot >= kBucketCount) {
        slot = kBucketCount - 1;
    }
    entry->wheel_level = 1;
    entry->wheel_slot = static_cast<uint32_t>(slot);
    wheel.level1[slot].push_back(entry);
}

void insert_entry(TimerEntry* entry) {
    uint64_t now = wheel.tick_count;
    uint64_t delta = (entry->deadline_ticks > now)
        ? (entry->deadline_ticks - now) : 0;

    if (delta < kBucketCount) {
        if (delta == 0) delta = 1;
        insert_level0(entry, delta);
    } else if (delta < kBucketCount * kBucketCount) {
        insert_level1(entry, delta);
    } else {
        entry->callback(entry->user_data);
        if (entry->period_ticks > 0) {
            entry->deadline_ticks = wheel.tick_count + entry->period_ticks;
            insert_entry(entry);
        } else {
            free_entry(entry);
        }
    }
}

void fire_entry(TimerEntry* entry) {
    entry->callback(entry->user_data);
    if (entry->period_ticks > 0) {
        entry->deadline_ticks = wheel.tick_count + entry->period_ticks;
        insert_entry(entry);
    } else {
        free_entry(entry);
    }
}

void cascade() {
    auto& bucket = wheel.level1[wheel.l1_cursor];
    while (!bucket.empty()) {
        TimerEntry* entry = bucket.pop_front();
        uint64_t now = wheel.tick_count;
        uint64_t remaining = (entry->deadline_ticks > now)
            ? (entry->deadline_ticks - now) : 0;

        if (remaining == 0) {
            fire_entry(entry);
        } else if (remaining < kBucketCount) {
            insert_level0(entry, remaining);
        } else {
            insert_level1(entry, remaining);
        }
    }
    wheel.l1_cursor = (wheel.l1_cursor + 1) % kBucketCount;
}

} // namespace

namespace tori::timer {

void init() {
    bsp_lapic_id = tori::arch::x86_64::lapic::id();

    for (size_t i = 0; i < CONFIG_TIMER_MAX_ENTRIES; ++i) {
        free_entries.push_back(&pool[i]);
    }

    initialized = true;
}

TimerEntry* create_oneshot(uint64_t deadline_ticks,
                           void (*callback)(void*), void* user_data) {
    if (!initialized || !callback) return nullptr;

    TimerEntry* entry = alloc_entry();
    if (!entry) return nullptr;

    entry->deadline_ticks = deadline_ticks;
    entry->period_ticks = 0;
    entry->callback = callback;
    entry->user_data = user_data;

    {
        tori::sync::IrqLockGuard guard(timer_lock);
        insert_entry(entry);
    }

    return entry;
}

TimerEntry* create_periodic(uint64_t start_delta, uint64_t period_ticks,
                            void (*callback)(void*), void* user_data) {
    if (!initialized || !callback || period_ticks == 0) return nullptr;

    TimerEntry* entry = alloc_entry();
    if (!entry) return nullptr;

    entry->deadline_ticks = wheel.tick_count + start_delta;
    entry->period_ticks = period_ticks;
    entry->callback = callback;
    entry->user_data = user_data;

    {
        tori::sync::IrqLockGuard guard(timer_lock);
        insert_entry(entry);
    }

    return entry;
}

void cancel(TimerEntry* entry) {
    if (!entry) return;

    tori::sync::IrqLockGuard guard(timer_lock);
    if (entry->wheel_level == 0) {
        wheel.level0[entry->wheel_slot].remove(entry);
    } else {
        wheel.level1[entry->wheel_slot].remove(entry);
    }
    free_entry(entry);
}

void tick() {
    if (!initialized) return;
    if (tori::arch::x86_64::lapic::id() != bsp_lapic_id) return;

    wheel.tick_count++;

    auto& bucket = wheel.level0[wheel.cursor];
    while (!bucket.empty()) {
        TimerEntry* entry = bucket.pop_front();
        fire_entry(entry);
    }

    wheel.cursor++;
    if (wheel.cursor == kBucketCount) {
        wheel.cursor = 0;
        cascade();
    }
}

uint64_t now() {
    return wheel.tick_count;
}

} // namespace tori::timer
