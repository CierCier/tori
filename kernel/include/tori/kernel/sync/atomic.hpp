#pragma once

#include <stdint.h>

namespace tori::sync {

enum class MemoryOrder {
    relaxed = __ATOMIC_RELAXED,
    consume = __ATOMIC_CONSUME,
    acquire = __ATOMIC_ACQUIRE,
    release = __ATOMIC_RELEASE,
    acq_rel = __ATOMIC_ACQ_REL,
    seq_cst = __ATOMIC_SEQ_CST
};

template<typename T>
class Atomic {
    static_assert(__atomic_always_lock_free(sizeof(T), 0), "Atomic<T> requires lock-free T");
    volatile T value_;

public:
    Atomic() : value_(T{}) {}
    explicit Atomic(T v) : value_(v) {}

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    T load(MemoryOrder order = MemoryOrder::seq_cst) const volatile {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }

    void store(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        __atomic_store_n(&value_, v, static_cast<int>(order));
    }

    T exchange(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        return __atomic_exchange_n(&value_, v, static_cast<int>(order));
    }

    bool compare_exchange(T& expected, T desired,
                          MemoryOrder succ = MemoryOrder::seq_cst,
                          MemoryOrder fail = MemoryOrder::seq_cst) volatile {
        return __atomic_compare_exchange_n(&value_, &expected, desired, false,
                                           static_cast<int>(succ), static_cast<int>(fail));
    }

    T fetch_add(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        return __atomic_fetch_add(&value_, v, static_cast<int>(order));
    }

    T fetch_sub(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        return __atomic_fetch_sub(&value_, v, static_cast<int>(order));
    }

    T fetch_and(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        return __atomic_fetch_and(&value_, v, static_cast<int>(order));
    }

    T fetch_or(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        return __atomic_fetch_or(&value_, v, static_cast<int>(order));
    }

    T fetch_xor(T v, MemoryOrder order = MemoryOrder::seq_cst) volatile {
        return __atomic_fetch_xor(&value_, v, static_cast<int>(order));
    }

    T operator++() volatile { return fetch_add(1) + 1; }
    T operator++(int) volatile { return fetch_add(1); }
    T operator--() volatile { return fetch_sub(1) - 1; }
    T operator--(int) volatile { return fetch_sub(1); }
};

} // namespace tori::sync
