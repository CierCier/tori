#pragma once

#include <stdint.h>

namespace tori::sync {

class Spinlock {
public:
    Spinlock() : locked_(0) {}

    // Non-copyable
    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void lock() {
        // Attempt to swap 1 into locked_. If it was already 1, keep spinning.
        while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
            // x86 PAUSE instruction to save power and improve spin performance
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

// RAII Guard for Spinlock
class LockGuard {
public:
    explicit LockGuard(Spinlock& lock) : lock_(lock) {
        lock_.lock();
    }
    ~LockGuard() {
        lock_.unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Spinlock& lock_;
};

} // namespace tori::sync
