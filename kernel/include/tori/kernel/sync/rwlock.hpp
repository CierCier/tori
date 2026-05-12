#pragma once

#include <stdint.h>

namespace tori::sync {

class RWLock {
public:
    RWLock() : state_(0) {}

    RWLock(const RWLock&) = delete;
    RWLock& operator=(const RWLock&) = delete;

    void lock_read() {
        while (true) {
            int s = __atomic_load_n(&state_, __ATOMIC_RELAXED);
            if (s >= 0) {
                if (__atomic_compare_exchange_n(&state_, &s, s + 1, false,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                    return;
                }
            }
            asm volatile("pause" ::: "memory");
        }
    }

    void unlock_read() {
        __atomic_sub_fetch(&state_, 1, __ATOMIC_RELEASE);
    }

    void lock_write() {
        while (true) {
            int s = 0;
            if (__atomic_compare_exchange_n(&state_, &s, -1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return;
            }
            asm volatile("pause" ::: "memory");
        }
    }

    bool try_lock_write() {
        int s = 0;
        return __atomic_compare_exchange_n(&state_, &s, -1, false,
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    void unlock_write() {
        __atomic_store_n(&state_, 0, __ATOMIC_RELEASE);
    }

private:
    volatile int state_;
};

} // namespace tori::sync
