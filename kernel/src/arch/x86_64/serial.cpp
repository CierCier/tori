#include "serial.hpp"

#include "io.hpp"

#include <stdint.h>

namespace {

constexpr uint16_t com1 = 0x3f8;

bool is_transmit_empty() {
    return (tori::arch::x86_64::inb(com1 + 5) & 0x20) != 0;
}

} // namespace

namespace tori::arch::x86_64::serial {

void init() {
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x80);
    outb(com1 + 0, 0x03);
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x03);
    outb(com1 + 2, 0xc7);
    outb(com1 + 4, 0x0b);
}

void write_char(char c) {
    while (!is_transmit_empty()) {
    }

    outb(com1, static_cast<uint8_t>(c));
}

void write_string(const char* text) {
    if (text == nullptr) {
        return;
    }

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            write_char('\r');
        }
        write_char(*cursor);
    }
}

} // namespace tori::arch::x86_64::serial
