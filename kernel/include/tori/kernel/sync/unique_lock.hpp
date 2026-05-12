#pragma once

#include <stdint.h>

namespace tori::sync {

struct defer_lock_t { explicit defer_lock_t() = default; };
struct try_to_lock_t { explicit try_to_lock_t() = default; };
struct adopt_lock_t { explicit adopt_lock_t() = default; };

inline constexpr defer_lock_t defer_lock{};
inline constexpr try_to_lock_t try_to_lock{};
inline constexpr adopt_lock_t adopt_lock{};

template<typename Lockable>
class UniqueLock {
public:
    UniqueLock() : lock_(nullptr), owns_(false) {}

    explicit UniqueLock(Lockable& lock) : lock_(&lock), owns_(true) {
        lock_->lock();
    }

    UniqueLock(Lockable& lock, defer_lock_t) : lock_(&lock), owns_(false) {}

    UniqueLock(Lockable& lock, try_to_lock_t) : lock_(&lock), owns_(lock.try_lock()) {}

    UniqueLock(Lockable& lock, adopt_lock_t) : lock_(&lock), owns_(true) {}

    ~UniqueLock() {
        if (owns_) {
            lock_->unlock();
        }
    }

    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;

    UniqueLock(UniqueLock&& other) : lock_(other.lock_), owns_(other.owns_) {
        other.lock_ = nullptr;
        other.owns_ = false;
    }

    UniqueLock& operator=(UniqueLock&& other) {
        if (this != &other) {
            if (owns_) lock_->unlock();
            lock_ = other.lock_;
            owns_ = other.owns_;
            other.lock_ = nullptr;
            other.owns_ = false;
        }
        return *this;
    }

    void lock() {
        lock_->lock();
        owns_ = true;
    }

    bool try_lock() {
        owns_ = lock_->try_lock();
        return owns_;
    }

    void unlock() {
        lock_->unlock();
        owns_ = false;
    }

    bool owns_lock() const { return owns_; }
    explicit operator bool() const { return owns_; }

    Lockable* release() {
        Lockable* l = lock_;
        lock_ = nullptr;
        owns_ = false;
        return l;
    }

    Lockable* mutex() const { return lock_; }

private:
    Lockable* lock_;
    bool owns_;
};

} // namespace tori::sync
