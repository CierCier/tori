#pragma once

#include <stdint.h>

namespace tori::sync {

class Semaphore {
public:
    explicit Semaphore(int initial_count) : count_(initial_count) {}

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    void wait() {
        while (true) {
            int cur = __atomic_load_n(&count_, __ATOMIC_RELAXED);
            if (cur > 0) {
                if (__atomic_compare_exchange_n(&count_, &cur, cur - 1, false,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                    return;
                }
            } else {
                asm volatile("pause" ::: "memory");
            }
        }
    }

    bool try_wait() {
        int cur = __atomic_load_n(&count_, __ATOMIC_RELAXED);
        while (cur > 0) {
            if (__atomic_compare_exchange_n(&count_, &cur, cur - 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return true;
            }
        }
        return false;
    }

    void post() {
        __atomic_add_fetch(&count_, 1, __ATOMIC_RELEASE);
    }

private:
    volatile int count_;
};

} // namespace tori::sync
