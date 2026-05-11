#include <stddef.h>
#include <stdint.h>

extern "C" void* memcpy(void* destination, const void* source, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(source);

    for (size_t index = 0; index < count; ++index) {
        dst[index] = src[index];
    }

    return destination;
}

extern "C" void* memset(void* destination, int value, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);

    for (size_t index = 0; index < count; ++index) {
        dst[index] = static_cast<uint8_t>(value);
    }

    return destination;
}

extern "C" void* memmove(void* destination, const void* source, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(source);

    if (dst < src) {
        for (size_t index = 0; index < count; ++index) {
            dst[index] = src[index];
        }
    } else if (dst > src) {
        for (size_t index = count; index > 0; --index) {
            dst[index - 1] = src[index - 1];
        }
    }

    return destination;
}
