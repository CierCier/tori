#include <tori/kernel/log.hpp>

#include <tori/kernel/lapic.hpp>
#include <tori/kernel/time.hpp>
#include <tori/kernel/timer.hpp>
#include <tori/kernel/sync/spinlock.hpp>
#include <tori/kernel/framebuffer_console.hpp>
#include "../arch/x86_64/serial.hpp"

namespace {

tori::sync::Spinlock log_lock;

constexpr unsigned color_trace = 0x00888888;
constexpr unsigned color_debug = 0x00aaaaaa;
constexpr unsigned color_info = 0x00ffffff;
constexpr unsigned color_warn = 0x00ffff00;
constexpr unsigned color_error = 0x00ff5555;
constexpr unsigned color_panic = 0x00ff00ff;

struct BootBuffer {
    tori::log::LogEntry entries[CONFIG_LOG_BOOT_ENTRIES];
    uint32_t write_idx;
    uint32_t read_idx;
};

BootBuffer boot_buffer = {};
tori::log::PerCpuBuffer per_cpu_buffers[CONFIG_MAX_CPUS] = {};
bool cpu_initialized[CONFIG_MAX_CPUS] = {};
bool any_cpu_initialized = false;

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

void format_entry(const tori::log::LogEntry& entry) {
    const unsigned color = level_color(entry.level);
    
    // Serial sink mask (Trace and above)
    // Framebuffer sink mask (Info and above)
    const bool to_serial = entry.level >= tori::log::Level::Trace;
    const bool to_fb = entry.level >= tori::log::Level::Info;

    if (!to_serial && !to_fb) return;

    // Hold the serial lock for the entire entry so that init's sys_write
    // cannot interleave fragments between multiple write_string calls.
    if (to_serial) tori::arch::x86_64::serial::lock();

    auto write_to_active_sinks = [&](const char* text, unsigned color) {
        if (to_serial) tori::arch::x86_64::serial::write_string_locked(text);
        if (to_fb) tori::log::framebuffer_console::write_string(text, color);
    };

    auto write_dec_active = [&](uint64_t value, unsigned color) {
        char buffer[21] = {};
        int cursor = 20;
        buffer[cursor] = '\0';
        if (value == 0) { write_to_active_sinks("0", color); return; }
        while (value != 0 && cursor > 0) {
            --cursor;
            buffer[cursor] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        write_to_active_sinks(&buffer[cursor], color);
    };

    auto write_hex_active = [&](uint64_t value, unsigned color) {
        constexpr char digits[] = "0123456789abcdef";
        char buffer[19] = {};
        buffer[0] = '0'; buffer[1] = 'x';
        for (int index = 0; index < 16; ++index) {
            const int shift = (15 - index) * 4;
            buffer[2 + index] = digits[(value >> shift) & 0xf];
        }
        buffer[18] = '\0';
        write_to_active_sinks(buffer, color);
    };

    write_to_active_sinks("[", color);
    write_to_active_sinks(level_name(entry.level), color);
    write_to_active_sinks("]", color);
    write_to_active_sinks("[", color);
    write_to_active_sinks(entry.category != nullptr ? entry.category : "kernel", color);
    write_to_active_sinks("] ", color);

    switch (entry.type) {
    case tori::log::EntryType::Message:
        write_to_active_sinks(entry.as_msg.msg != nullptr ? entry.as_msg.msg : "(null)", color);
        break;
    case tori::log::EntryType::Value:
        write_to_active_sinks(entry.as_value.label != nullptr ? entry.as_value.label : "value", color);
        write_to_active_sinks(" = ", color);
        write_hex_active(entry.as_value.value, color);
        write_to_active_sinks(" (", color);
        write_dec_active(entry.as_value.value, color);
        write_to_active_sinks(")", color);
        break;
    case tori::log::EntryType::TextValue:
        write_to_active_sinks(entry.as_text.label != nullptr ? entry.as_text.label : "value", color);
        write_to_active_sinks(" = ", color);
        write_to_active_sinks(entry.as_text.value != nullptr ? entry.as_text.value : "(null)", color);
        break;
    }

    write_to_active_sinks("\n", color);

    if (to_serial) tori::arch::x86_64::serial::unlock();
}

void push_entry(const tori::log::LogEntry& entry) {
    uint64_t rflags;
    asm volatile("pushfq; popq %0" : "=r"(rflags) : : "memory");
    asm volatile("cli" : : : "memory");

    uint32_t lapic_id = 0;
    bool use_per_cpu = false;
    
    // We can only use LAPIC ID if we've initialized any CPU.
    // Early boot messages go to boot_buffer.
    if (any_cpu_initialized) {
        lapic_id = tori::arch::x86_64::lapic::id();
        if (lapic_id < CONFIG_MAX_CPUS && cpu_initialized[lapic_id]) {
            use_per_cpu = true;
        }
    }

    if (use_per_cpu) {
        auto& buf = per_cpu_buffers[lapic_id];
        uint32_t next = (buf.write_idx + 1) % CONFIG_LOG_RING_ENTRIES;
        
        // If buffer is full, we must flush synchronously to make room.
        // Handoff says: "If buffer >75% full, flush inline (backpressure)"
        // But simpler is to flush if next == read_idx.
        // Let's implement the 75% backpressure.
        uint32_t used = (buf.write_idx >= buf.read_idx) 
            ? (buf.write_idx - buf.read_idx) 
            : (CONFIG_LOG_RING_ENTRIES - (buf.read_idx - buf.write_idx));
        
        if (used > (CONFIG_LOG_RING_ENTRIES * 3 / 4)) {
            tori::log::flush();
        }

        if (next != buf.read_idx) {
            buf.entries[buf.write_idx] = entry;
            buf.write_idx = next;
        }
    } else {
        // Boot buffer uses a spinlock because multiple CPUs might be in early init
        // (though usually it's just the BSP).
        tori::sync::LockGuard guard(log_lock);
        uint32_t next = (boot_buffer.write_idx + 1) % CONFIG_LOG_BOOT_ENTRIES;
        if (next != boot_buffer.read_idx) {
            boot_buffer.entries[boot_buffer.write_idx] = entry;
            boot_buffer.write_idx = next;
        }
    }

    if (rflags & (1 << 9)) {
        asm volatile("sti" : : : "memory");
    }
}

} // namespace

namespace tori::log {

void init_serial() {
    arch::x86_64::serial::init();
}

void init_framebuffer(const boot::Framebuffer& framebuffer) {
    framebuffer_console::init(framebuffer);
}

void write(Level level, const char* category, const char* file, int line, const char* message) {
    LogEntry entry = {
        .timestamp = tori::time::uptime_ms(),
        .level = level,
        .type = EntryType::Message,
        .category = category,
        .file = file,
        .line = static_cast<int16_t>(line),
        .as_msg = { .msg = message }
    };
    push_entry(entry);
}

void write_value(Level level, const char* category, const char* file, int line, const char* label, uint64_t value) {
    LogEntry entry = {
        .timestamp = tori::time::uptime_ms(),
        .level = level,
        .type = EntryType::Value,
        .category = category,
        .file = file,
        .line = static_cast<int16_t>(line),
        .as_value = { .label = label, .value = value }
    };
    push_entry(entry);
}

void write_text_value(Level level, const char* category, const char* file, int line, const char* label, const char* value) {
    LogEntry entry = {
        .timestamp = tori::time::uptime_ms(),
        .level = level,
        .type = EntryType::TextValue,
        .category = category,
        .file = file,
        .line = static_cast<int16_t>(line),
        .as_text = { .label = label, .value = value }
    };
    push_entry(entry);
}

void flush() {
    // Only one flusher at a time to avoid interleaved output on sinks
    if (!log_lock.try_lock()) return;

    // 1. Drain boot buffer first
    while (boot_buffer.read_idx != boot_buffer.write_idx) {
        format_entry(boot_buffer.entries[boot_buffer.read_idx]);
        boot_buffer.read_idx = (boot_buffer.read_idx + 1) % CONFIG_LOG_BOOT_ENTRIES;
    }

    // 2. Drain current CPU buffer
    if (any_cpu_initialized) {
        uint32_t lapic_id = tori::arch::x86_64::lapic::id();
        if (lapic_id < CONFIG_MAX_CPUS && cpu_initialized[lapic_id]) {
            auto& buf = per_cpu_buffers[lapic_id];
            uint32_t count = 0;
            while (buf.read_idx != buf.write_idx && count < CONFIG_LOG_FLUSH_BATCH) {
                format_entry(buf.entries[buf.read_idx]);
                buf.read_idx = (buf.read_idx + 1) % CONFIG_LOG_RING_ENTRIES;
                count++;
            }
        }
    }

    log_lock.unlock();
}

void flush_all() {
    if (!log_lock.try_lock()) return;

    while (boot_buffer.read_idx != boot_buffer.write_idx) {
        format_entry(boot_buffer.entries[boot_buffer.read_idx]);
        boot_buffer.read_idx = (boot_buffer.read_idx + 1) % CONFIG_LOG_BOOT_ENTRIES;
    }
    log_lock.unlock();

    if (any_cpu_initialized) {
        uint32_t lapic_id = tori::arch::x86_64::lapic::id();
        if (lapic_id < CONFIG_MAX_CPUS && cpu_initialized[lapic_id]) {
            auto& buf = per_cpu_buffers[lapic_id];
            while (buf.read_idx != buf.write_idx) {
                format_entry(buf.entries[buf.read_idx]);
                buf.read_idx = (buf.read_idx + 1) % CONFIG_LOG_RING_ENTRIES;
            }
        }
    }
}

static void log_flush_timer_callback(void*) {
    flush_all();
}

void init_timer_flush() {
    tori::timer::create_periodic(
        CONFIG_LOG_FLUSH_PERIOD_TICKS,
        CONFIG_LOG_FLUSH_PERIOD_TICKS,
        &log_flush_timer_callback,
        nullptr
    );
}

void init_per_cpu(uint32_t lapic_id) {
    if (lapic_id >= CONFIG_MAX_CPUS) return;

    // Transition: drain boot buffer synchronously
    tori::sync::LockGuard guard(log_lock);
    while (boot_buffer.read_idx != boot_buffer.write_idx) {
        format_entry(boot_buffer.entries[boot_buffer.read_idx]);
        boot_buffer.read_idx = (boot_buffer.read_idx + 1) % CONFIG_LOG_BOOT_ENTRIES;
    }

    cpu_initialized[lapic_id] = true;
    any_cpu_initialized = true;
}

void print_stack_trace_from(uint64_t rip, uint64_t rsp, uint64_t rbp) {
    uint64_t frames[32];
    int count = tori::stacktrace::collect(rip, rsp, rbp, frames, 32);
    if (count <= 0) return;

    write_sink("\nStack trace:\n", color_panic);
    for (int i = 0; i < count; ++i) {
        write_sink("  #", color_panic);
        write_dec(i, color_panic);
        write_sink(" [", color_panic);

        uint64_t offset;
        const char* name = tori::stacktrace::resolve(frames[i], offset);
        if (name) {
            write_sink(name, color_panic);
            if (offset > 0) {
                write_sink("+", color_panic);
                write_hex(offset, color_panic);
            }
        } else {
            write_hex(frames[i], color_panic);
        }
        write_sink("]\n", color_panic);
    }
}

void print_stack_trace() {
    uint64_t frame_rbp;
    asm volatile("mov %%rbp, %0" : "=r"(frame_rbp));
    uint64_t caller_rbp = *(uint64_t*)frame_rbp;
    uint64_t caller_rip = *(uint64_t*)(frame_rbp + 8);
    uint64_t caller_rsp = frame_rbp + 16;
    print_stack_trace_from(caller_rip, caller_rsp, caller_rbp);
}

[[noreturn]] void panic(const char* category, const char* file, int line, const char* message) {
    // Synchronously drain everything
    tori::sync::LockGuard guard(log_lock);
    
    // Boot buffer
    while (boot_buffer.read_idx != boot_buffer.write_idx) {
        format_entry(boot_buffer.entries[boot_buffer.read_idx]);
        boot_buffer.read_idx = (boot_buffer.read_idx + 1) % CONFIG_LOG_BOOT_ENTRIES;
    }

    // All CPU buffers
    for (size_t i = 0; i < CONFIG_MAX_CPUS; ++i) {
        if (cpu_initialized[i]) {
            auto& buf = per_cpu_buffers[i];
            while (buf.read_idx != buf.write_idx) {
                format_entry(buf.entries[buf.read_idx]);
                buf.read_idx = (buf.read_idx + 1) % CONFIG_LOG_RING_ENTRIES;
            }
        }
    }

    // Final direct message
    const unsigned color = color_panic;
    write_prefix(Level::Panic, category, color);
    write_sink(message != nullptr ? message : "(panic)", color);
    write_sink("\n", color);
    write_sink("Location: ", color);
    write_sink(file, color);
    write_sink(":", color);
    write_dec(line, color);
    write_sink("\n", color);

    // Print stack trace from caller's context
    uint64_t frame_rbp;
    asm volatile("mov %%rbp, %0" : "=r"(frame_rbp));
    uint64_t caller_rbp = *(uint64_t*)frame_rbp;
    uint64_t caller_rip = *(uint64_t*)(frame_rbp + 8);
    uint64_t caller_rsp = frame_rbp + 16;
    print_stack_trace_from(caller_rip, caller_rsp, caller_rbp);

    for (;;) {
        asm volatile("cli; hlt" : : : "memory");
    }
}

} // namespace tori::log

extern "C" void log_flush() {
    tori::log::flush();
}
