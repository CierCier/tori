#pragma once

#include <stdint.h>

#include <tori/kernel/boot_info.hpp>
#include <tori/kernel/stacktrace.hpp>
#include <config.h>

namespace tori::log {

enum class Level : uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Panic,
};

void init_serial();
void init_framebuffer(const boot::Framebuffer& framebuffer);

void write(Level level, const char* category, const char* file, int line, const char* message);
void write_value(Level level, const char* category, const char* file, int line, const char* label, uint64_t value);
void write_text_value(Level level, const char* category, const char* file, int line, const char* label, const char* value);
[[noreturn]] void panic(const char* category, const char* file, int line, const char* message);

void print_stack_trace();
void print_stack_trace_from(uint64_t rip, uint64_t rsp, uint64_t rbp);

void init_per_cpu(uint32_t lapic_id);
void flush();
void flush_all();
void init_timer_flush();

enum class EntryType : uint8_t {
    Message,
    Value,
    TextValue,
};

struct LogEntry {
    uint64_t timestamp;
    Level level;
    EntryType type;
    const char* category;
    const char* file;
    int16_t line;
    union {
        struct {
            const char* msg;
        } as_msg;
        struct {
            const char* label;
            uint64_t value;
        } as_value;
        struct {
            const char* label;
            const char* value;
        } as_text;
    };
};

struct PerCpuBuffer {
    LogEntry entries[CONFIG_LOG_RING_ENTRIES];
    uint32_t write_idx;
    uint32_t read_idx;
};

} // namespace tori::log

extern "C" void log_flush();

#define TORI_LOG_TRACE(category, message) ::tori::log::write(::tori::log::Level::Trace, category, __FILE__, __LINE__, message)
#define TORI_LOG_DEBUG(category, message) ::tori::log::write(::tori::log::Level::Debug, category, __FILE__, __LINE__, message)
#define TORI_LOG_INFO(category, message) ::tori::log::write(::tori::log::Level::Info, category, __FILE__, __LINE__, message)
#define TORI_LOG_WARN(category, message) ::tori::log::write(::tori::log::Level::Warn, category, __FILE__, __LINE__, message)
#define TORI_LOG_ERROR(category, message) ::tori::log::write(::tori::log::Level::Error, category, __FILE__, __LINE__, message)
#define TORI_LOG_VALUE(level, category, label, value) ::tori::log::write_value(level, category, __FILE__, __LINE__, label, value)
#define TORI_LOG_TEXT_VALUE(level, category, label, value) ::tori::log::write_text_value(level, category, __FILE__, __LINE__, label, value)
#define TORI_PANIC(category, message) ::tori::log::panic(category, __FILE__, __LINE__, message)
