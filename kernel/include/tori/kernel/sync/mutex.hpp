#pragma once

#include <stdint.h>

namespace tori::sync {

class Mutex {
public:
    Mutex() : locked_(0) {}

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
            asm volatile("pause" ::: "memory");
        }
    }

    bool try_lock() {
        return __atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE) == 0;
    }

    void unlock() {
        __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE);
    }

private:
    volatile int locked_;
};

} // namespace tori::sync
