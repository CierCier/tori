#pragma once

#include <stdint.h>
#include <tori/kernel/time.hpp>

namespace tori::sync {

class ConditionVariable {
public:
    ConditionVariable() = default;

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    template<typename Lockable, typename Predicate>
    void wait(Lockable& lock, Predicate pred) {
        while (!pred()) {
            lock.unlock();
            asm volatile("pause" ::: "memory");
            lock.lock();
        }
    }

    template<typename Lockable, typename Predicate>
    bool wait_for(Lockable& lock, Predicate pred, uint64_t timeout_ms) {
        uint64_t end = tori::time::uptime_ms() + timeout_ms;
        while (!pred()) {
            if (tori::time::uptime_ms() >= end) {
                return false;
            }
            lock.unlock();
            asm volatile("pause" ::: "memory");
            lock.lock();
        }
        return true;
    }

    template<typename Lockable, typename Predicate>
    bool wait_until(Lockable& lock, Predicate pred, uint64_t abs_ms) {
        while (!pred()) {
            if (tori::time::uptime_ms() >= abs_ms) {
                return false;
            }
            lock.unlock();
            asm volatile("pause" ::: "memory");
            lock.lock();
        }
        return true;
    }

    void notify_one() {}
    void notify_all() {}
};

} // namespace tori::sync
