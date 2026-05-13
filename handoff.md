# Handoff: Per-CPU Ring Buffer Logging

## Goal

Replace synchronous sink-direct logging with per-CPU ring buffers and deferred flushing. Each log write stores a structured entry in the local CPU's ring buffer; a timer-tick partial flush drains entries to serial and framebuffer sinks. Panic flushes the buffer synchronously before halting.

---

## Architecture

### Two Buffer Stages

1. **Boot buffer** — small global static array (64 entries), used before per-CPU setup. Captures early log messages (before `time::init`, before per-CPU arrays exist).
2. **Per-CPU ring buffers** — each CPU gets its own buffer (256 entries). No lock contention on write.

**Transition**: when `log::init_per_cpu()` is called, replay boot buffer through sinks, then switch this CPU to its per-CPU buffer.

---

### LogEntry (structured record stored in ring buffer)

```cpp
enum class EntryType : uint8_t { Message, Value, TextValue };

struct LogEntry {
    uint64_t timestamp;         // time::uptime_ms(), 0 before time init
    Level    level;
    EntryType type;
    const char* category;       // string literal, always valid
    const char* file;           // __FILE__ literal
    int16_t   line;
    union {
        struct { const char* msg; } as_msg;
        struct { const char* label; uint64_t value; } as_value;
        struct { const char* label; const char* value; } as_text;
    };
};
```

Size: ~56 bytes. 256 entries × 56 bytes = ~14 KB per CPU. Pointers safe — all passed via macros, live in `.rodata`.

### Per-CPU Buffer

```cpp
struct PerCpuBuffer {
    LogEntry entries[CONFIG_LOG_RING_ENTRIES];  // 256, configurable
    uint32_t write_idx;   // written by owning CPU only
    uint32_t read_idx;    // advanced by flusher
};
```

Write index: single writer (owning CPU only, interrupts disabled), no lock.
Read index: read by flusher, advanced after entries are drained.

### Sink Masks

```cpp
constexpr Level serial_min = Level::Trace;   // serial gets everything
constexpr Level fb_min     = Level::Info;     // fb skips trace/debug
```

Flush checks each entry's level against each sink's range before formatting.

---

## Write Path (buffered mode)

```
write(level, category, file, line, message):
  1. Get current CPU's buffer (or boot buffer if pre-init)
  2. Fill next entry at write_idx
  3. Advance write_idx (no lock, single writer)
  4. If buffer >75% full, flush inline (backpressure)
```

Fast path: 1 struct store + 1 index increment. No I/O.

`write_value()` and `write_text_value()` follow the same pattern with their respective union fields.

---

## Flush Path (partial, in timer ISR)

```
log_flush():
  1. Get current CPU's buffer
  2. For up to CONFIG_LOG_FLUSH_BATCH entries (default 4):
     a. Read entry at read_idx
     b. For each sink (serial, fb):
        - Check level mask (entry.level >= sink.min_level)
        - Format entry into log line + dispatch to sink
     c. Advance read_idx
  3. If buffer still has entries, set need_flush hint
```

Formatting happens at flush time, not write time. The format-and-dispatch logic from today's `write()`, `write_value()`, `write_text_value()` is factored into functions that take a `LogEntry` and a sink function pointer (serial `write_string` / fb `write_string`). No new formatting code; existing helpers (`write_prefix`, `write_dec`, `write_hex`, `level_name`, `level_color`) are reused.

Timer ISR calls `log_flush()` after `sched_do_preempt()`. Bounded work: at most 4 entries × 2 sinks = 8 writes. No reentrancy concern — timer ISR runs with interrupts disabled on this CPU.

### Panic Path

```
panic():
  1. Drain entire ring buffer for current CPU (flush all entries synchronously)
  2. Write panic message directly
  3. Infinite cli; hlt
```

### Boot Replay

```
init_per_cpu(lapic_id):
  1. Drain boot buffer through sink dispatch (all unflushed entries)
  2. Point this CPU to its per-CPU buffer
  3. Future writes go to per-CPU buffer
```

---

## File Changes

| File | Change |
|---|---|
| `kernel/include/tori/kernel/log.hpp` | Add `LogEntry`, `EntryType`, `PerCpuBuffer`, `flush()`, `init_per_cpu()`, config constants |
| `kernel/src/log/log.cpp` | Refactor write functions to store entries. Extract format-logic for flush path. Add boot buffer + per-CPU buffers + partial flush + sink mask checks |
| `kernel/src/log/framebuffer_console.cpp` | No change (sink interface unchanged) |
| `kernel/src/arch/x86_64/serial.cpp` | No change |
| `kernel/src/arch/x86_64/interrupts_asm.S` | Add `call log_flush` after `call sched_do_preempt` |
| `kernel/src/kernel/task.cpp` | Add `log_flush()` call in `idle_entry()` as safety net |
| `kernel/src/kernel/main.cpp` | Add `log::init_per_cpu()` call after `time::init()` |
| `kernel/config.h` | Add `CONFIG_LOG_RING_ENTRIES` (256), `CONFIG_LOG_FLUSH_BATCH` (4), `CONFIG_LOG_BOOT_ENTRIES` (64) |

---

## Testing

- Boot kernel, verify all boot messages appear on serial and framebuffer (same as today)
- Verify no lock contention or crashes with 4 CPUs logging simultaneously
- Verify panic flushes remaining buffer before halt
- Verify timer ISR partial flush does not cause visible log loss (may show stale entries briefly)
