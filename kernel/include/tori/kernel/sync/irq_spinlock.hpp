#pragma once

#include <stdint.h>

namespace tori::sync {

class IrqSpinlock {
public:
    IrqSpinlock() : locked_(0) {}

    IrqSpinlock(const IrqSpinlock&) = delete;
    IrqSpinlock& operator=(const IrqSpinlock&) = delete;

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

class IrqLockGuard {
public:
    explicit IrqLockGuard(IrqSpinlock& lock) : lock_(lock), rflags_(0) {
        asm volatile(
            "pushfq\n\t"
            "popq %0\n\t"
            "cli\n\t"
            : "=r"(rflags_)
            :
            : "memory"
        );
        lock_.lock();
    }

    ~IrqLockGuard() {
        lock_.unlock();
        if (rflags_ & 0x200) {
            asm volatile("sti" ::: "memory");
        }
    }

    IrqLockGuard(const IrqLockGuard&) = delete;
    IrqLockGuard& operator=(const IrqLockGuard&) = delete;

private:
    IrqSpinlock& lock_;
    uint64_t rflags_;
};

} // namespace tori::sync
