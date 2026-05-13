#pragma once

#include <stdint.h>
#include <stddef.h>

#include <tori/kernel/container/intrusive_list.hpp>
#include <config.h>

namespace tori::timer {

struct TimerEntry {
    container::IntrusiveListNode wheel_node;
    uint64_t deadline_ticks;
    uint64_t period_ticks;
    void (*callback)(void*);
    void* user_data;
    uint32_t wheel_level : 1;
    uint32_t wheel_slot  : 8;
};

using TimerBucket = container::IntrusiveList<TimerEntry, offsetof(TimerEntry, wheel_node)>;

void init();
TimerEntry* create_oneshot(uint64_t deadline_ticks,
                           void (*callback)(void*), void* user_data);
TimerEntry* create_periodic(uint64_t start_delta, uint64_t period_ticks,
                            void (*callback)(void*), void* user_data);
void cancel(TimerEntry* entry);
void tick();
uint64_t now();

inline constexpr uint64_t ms(uint64_t ms) { return ms; }

} // namespace tori::timer
