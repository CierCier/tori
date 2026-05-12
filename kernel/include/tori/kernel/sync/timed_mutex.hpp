#pragma once

#include <tori/kernel/sync/mutex.hpp>
#include <tori/kernel/time.hpp>

namespace tori::sync {

class TimedMutex : public Mutex {
public:
    using Mutex::Mutex;

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

    bool try_lock_until(uint64_t abs_ms) {
        while (!try_lock()) {
            if (tori::time::uptime_ms() >= abs_ms) {
                return false;
            }
            asm volatile("pause" ::: "memory");
        }
        return true;
    }
};

} // namespace tori::sync
