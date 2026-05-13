#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::container {

template<typename T>
class Optional {
    static_assert(__is_trivially_copyable(T), "Optional requires trivially copyable type");
public:
    Optional() : value_{}, has_value_(false) {}
    Optional(const T& v) : value_(v), has_value_(true) {}

    Optional(const Optional&) = default;
    Optional& operator=(const Optional&) = default;

    explicit operator bool() const { return has_value_; }

    T& value() { return value_; }
    const T& value() const { return value_; }

    T& operator*() { return value_; }
    const T& operator*() const { return value_; }

    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }

private:
    T value_;
    bool has_value_;
};

} // namespace tori::container
