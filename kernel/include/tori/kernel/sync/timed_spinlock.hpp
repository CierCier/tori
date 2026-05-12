#pragma once

#include <tori/kernel/sync/spinlock.hpp>
#include <tori/kernel/time.hpp>

namespace tori::sync {

class TimedSpinlock : public Spinlock {
public:
    using Spinlock::Spinlock;

    // Attempts to acquire the lock for at most timeout_ms.
    // Returns true on success, false on timeout.
    bool try_lock_for(uint64_t timeout_ms) {
        uint64_t end = tori::time::uptime_ms() + timeout_ms;
        
        while (!try_lock()) {
            if (tori::time::uptime_ms() >= end) {
                return false;
            }
            asm volatile("pause" ::: "memory");
        }
        return true;
    }
};

} // namespace tori::sync
