#pragma once

#include <tori/kernel/sync/semaphore.hpp>
#include <tori/kernel/time.hpp>

namespace tori::sync {

class TimedSemaphore : public Semaphore {
public:
    using Semaphore::Semaphore;
    using Semaphore::try_wait;

    bool wait_for(uint64_t timeout_ms) {
        uint64_t end = tori::time::uptime_ms() + timeout_ms;

        while (true) {
            if (try_wait()) {
                return true;
            }
            if (tori::time::uptime_ms() >= end) {
                return false;
            }
            asm volatile("pause" ::: "memory");
        }
    }

    bool wait_until(uint64_t abs_ms) {
        while (true) {
            if (try_wait()) {
                return true;
            }
            if (tori::time::uptime_ms() >= abs_ms) {
                return false;
            }
            asm volatile("pause" ::: "memory");
        }
    }
};

} // namespace tori::sync
