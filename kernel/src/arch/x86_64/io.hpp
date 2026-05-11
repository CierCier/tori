#pragma once

#include <stdint.h>

namespace tori::arch::x86_64 {

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

inline uint8_t inb(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

} // namespace tori::arch::x86_64
