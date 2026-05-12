#pragma once

#include <stdint.h>

namespace tori::sync {

class Seqlock {
public:
    Seqlock() : seq_(0) {}

    Seqlock(const Seqlock&) = delete;
    Seqlock& operator=(const Seqlock&) = delete;

    uint64_t read_begin() {
        uint64_t s;
        while (true) {
            s = __atomic_load_n(&seq_, __ATOMIC_ACQUIRE);
            if ((s & 1) == 0) {
                return s;
            }
            asm volatile("pause" ::: "memory");
        }
    }

    bool read_retry(uint64_t s) {
        asm volatile("" ::: "memory");
        return __atomic_load_n(&seq_, __ATOMIC_ACQUIRE) != s;
    }

    void write_begin() {
        uint64_t s = __atomic_load_n(&seq_, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&seq_, &s, s + 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            asm volatile("pause" ::: "memory");
        }
    }

    void write_end() {
        __atomic_store_n(&seq_, seq_ + 1, __ATOMIC_RELEASE);
    }

private:
    volatile uint64_t seq_;
};

} // namespace tori::sync
