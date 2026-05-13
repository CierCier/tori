#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::memory {

inline size_t string_length(const char* s) {
    size_t len = 0;
    while (s[len]) ++len;
    return len;
}

inline bool string_equals(const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}

inline void copy_string(char* dst, const char* src, size_t max_len) {
    size_t i = 0;
    while (i < max_len - 1 && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

inline void copy_memory(void* dst, const void* src, size_t size) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < size; ++i) d[i] = s[i];
}

inline void zero_memory(void* dst, size_t size) {
    auto* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < size; ++i) d[i] = 0;
}

} // namespace tori::memory
