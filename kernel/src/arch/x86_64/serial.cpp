#include "serial.hpp"

#include "io.hpp"

#include <tori/kernel/sync/spinlock.hpp>

#include <stdint.h>

namespace {

constexpr uint16_t com1 = 0x3f8;

tori::sync::Spinlock serial_lock;

bool is_transmit_empty() {
    return (tori::arch::x86_64::inb(com1 + 5) & 0x20) != 0;
}

} // namespace

namespace tori::arch::x86_64::serial {

// Write a single character without acquiring the serial lock
// (caller must hold serial_lock).
void write_char_locked(char c) {
    while (!is_transmit_empty()) {
    }
    outb(com1, static_cast<uint8_t>(c));
}

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
    tori::sync::LockGuard guard(serial_lock);
    write_char_locked(c);
}

void write_string(const char* text) {
    if (text == nullptr) {
        return;
    }

    tori::sync::LockGuard guard(serial_lock);
    write_string_locked(text);
}

void lock() {
    serial_lock.lock();
}

void unlock() {
    serial_lock.unlock();
}

void write_string_locked(const char* text) {
    if (text == nullptr) {
        return;
    }

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            write_char_locked('\r');
        }
        write_char_locked(*cursor);
    }
}

} // namespace tori::arch::x86_64::serial
