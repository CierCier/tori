#include <tori/kernel/log.hpp>

#include <tori/kernel/sync/spinlock.hpp>
#include "framebuffer_console.hpp"
#include "../arch/x86_64/serial.hpp"

namespace {

tori::sync::Spinlock log_lock;

constexpr unsigned color_trace = 0x00888888;
constexpr unsigned color_debug = 0x00aaaaaa;
constexpr unsigned color_info = 0x00ffffff;
constexpr unsigned color_warn = 0x00ffff00;
constexpr unsigned color_error = 0x00ff5555;
constexpr unsigned color_panic = 0x00ff00ff;

const char* level_name(tori::log::Level level) {
    switch (level) {
    case tori::log::Level::Trace:
        return "TRACE";
    case tori::log::Level::Debug:
        return "DEBUG";
    case tori::log::Level::Info:
        return "INFO";
    case tori::log::Level::Warn:
        return "WARN";
    case tori::log::Level::Error:
        return "ERROR";
    case tori::log::Level::Panic:
        return "PANIC";
    }

    return "LOG";
}

unsigned level_color(tori::log::Level level) {
    switch (level) {
    case tori::log::Level::Trace:
        return color_trace;
    case tori::log::Level::Debug:
        return color_debug;
    case tori::log::Level::Info:
        return color_info;
    case tori::log::Level::Warn:
        return color_warn;
    case tori::log::Level::Error:
        return color_error;
    case tori::log::Level::Panic:
        return color_panic;
    }

    return color_info;
}

void write_sink(const char* text, unsigned color) {
    tori::arch::x86_64::serial::write_string(text);
    tori::log::framebuffer_console::write_string(text, color);
}

void write_dec(uint64_t value, unsigned color) {
    char buffer[21] = {};
    int cursor = 20;
    buffer[cursor] = '\0';

    if (value == 0) {
        write_sink("0", color);
        return;
    }

    while (value != 0 && cursor > 0) {
        --cursor;
        buffer[cursor] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    write_sink(&buffer[cursor], color);
}

void write_hex(uint64_t value, unsigned color) {
    constexpr char digits[] = "0123456789abcdef";
    char buffer[19] = {};
    buffer[0] = '0';
    buffer[1] = 'x';

    for (int index = 0; index < 16; ++index) {
        const int shift = (15 - index) * 4;
        buffer[2 + index] = digits[(value >> shift) & 0xf];
    }

    buffer[18] = '\0';
    write_sink(buffer, color);
}

void write_prefix(tori::log::Level level, const char* category, unsigned color) {
    write_sink("[", color);
    write_sink(level_name(level), color);
    write_sink("]", color);
    write_sink("[", color);
    write_sink(category != nullptr ? category : "kernel", color);
    write_sink("] ", color);
}

} // namespace

namespace tori::log {

void init_serial() {
    arch::x86_64::serial::init();
}

void init_framebuffer(const boot::Framebuffer& framebuffer) {
    framebuffer_console::init(framebuffer);
}

void write(Level level, const char* category, const char*, int, const char* message) {
    tori::sync::LockGuard guard(log_lock);
    const unsigned color = level_color(level);
    write_prefix(level, category, color);
    write_sink(message != nullptr ? message : "(null)", color);
    write_sink("\n", color);
}

void write_value(Level level, const char* category, const char*, int, const char* label, uint64_t value) {
    tori::sync::LockGuard guard(log_lock);
    const unsigned color = level_color(level);
    write_prefix(level, category, color);
    write_sink(label != nullptr ? label : "value", color);
    write_sink(" = ", color);
    write_hex(value, color);
    write_sink(" (", color);
    write_dec(value, color);
    write_sink(")\n", color);
}

void write_text_value(Level level, const char* category, const char*, int, const char* label, const char* value) {
    tori::sync::LockGuard guard(log_lock);
    const unsigned color = level_color(level);
    write_prefix(level, category, color);
    write_sink(label != nullptr ? label : "value", color);
    write_sink(" = ", color);
    write_sink(value != nullptr ? value : "(null)", color);
    write_sink("\n", color);
}

[[noreturn]] void panic(const char* category, const char* file, int line, const char* message) {
    write(Level::Panic, category, file, line, message);
    for (;;) {
        asm volatile("cli; hlt" : : : "memory");
    }
}

} // namespace tori::log
