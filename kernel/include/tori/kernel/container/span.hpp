#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::container {

template<typename T>
struct Span {
    T* data;
    size_t size;

    T& operator[](size_t i) { return data[i]; }
    const T& operator[](size_t i) const { return data[i]; }

    T* begin() { return data; }
    T* end() { return data + size; }

    const T* begin() const { return data; }
    const T* end() const { return data + size; }

    bool empty() const { return size == 0; }
};

} // namespace tori::container
