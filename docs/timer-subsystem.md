# Timer Subsystem

## Summary

Generic kernel timer API backed by a two-level cascading timer wheel. First consumer: periodic 8ms log buffer flush, decoupling flush frequency from scheduling tick rate.

## Generic Container Layer

Three reusable templates in `kernel/include/tori/kernel/container/`:

- **`IntrusiveList<T, Node T::*>`** — doubly-linked list where the node is embedded in `T`. Member-pointer parameter lets a struct participate in multiple lists (each with a different node field). O(1) push/pop/remove.
- **`Optional<T>`** — trivially-relocatable nullable wrapper. Placement-new construction in aligned storage.
- **`Span<T>`** — pointer + size view over contiguous memory.

## Timer Wheel Architecture

Two-level cascading wheel driven by a monotonically increasing `uint64_t` tick counter:

- Level 0: 256 buckets, 1 tick each — covers 0..255 ticks ahead
- Level 1: 256 buckets, 256 ticks each — covers 256..65535 ticks ahead
- Each bucket is an `IntrusiveList<TimerEntry>` head

### Tick Advance (`timer::tick()`)

Called once per tick from the BSP's LAPIC timer ISR:

1. Fire level 0: drain current bucket at `cursor`. Invoke each timer's callback. Re-arm periodic timers.
2. Cascade: if `cursor` wrapped to 0 (256 ticks elapsed), advance level 1 by one bucket. Take all timers from that bucket and re-insert each into the appropriate level-0 bucket.
3. Advance: `cursor = (cursor + 1) % 256`

### TimerEntry

```cpp
struct TimerEntry {
    IntrusiveListNode wheel_node;
    uint64_t          deadline_ticks;
    uint64_t          period_ticks;     // 0 = one-shot
    void            (*callback)(void*);
    void*             user_data;
    uint32_t          wheel_level : 1;
    uint32_t          wheel_slot  : 8;
};
```

### API

```cpp
namespace tori::timer {

void init();
TimerEntry* create_oneshot(uint64_t deadline_ticks,
                           void (*callback)(void*), void* user_data);
TimerEntry* create_periodic(uint64_t start_delta, uint64_t period_ticks,
                            void (*callback)(void*), void* user_data);
void cancel(TimerEntry* entry);
void tick();
uint64_t now();

} // namespace tori::timer
```

### ISR Safety

- `tick()` called from BSP LAPIC timer ISR with interrupts disabled
- Callbacks fire in ISR context — must not block, allocate, or self-deadlock
- `create_*`/`cancel` use `IrqSpinlock` for task-context safety

## Integration

- `timer::init()` called in `kernel_main()` after `time::init()`
- `timer::tick()` called in `handle_interrupt()` (vector 32 handler in `idt.cpp`) on BSP only
- `timer::init_per_cpu()` if per-CPU timers are added later

## First Consumer: Periodic Log Flush

Replace the tick-coupled drain in `log_flush()` with a dedicated 8ms periodic timer:

- Register a periodic timer with period 8 ticks (8ms when 1 tick = 1ms)
- Callback drains the full current-CPU ring buffer (not just CONFIG_LOG_FLUSH_BATCH entries)
- Remove the batch-limited drain from the ISR common handler's `log_flush()` call
- Keep `log_flush()` call in ISR handler for non-BSP CPUs as a safety net

## Future

- Higher-resolution time base via finer clock source (µs/ns) — wheel structure unchanged, just tick size changes
- More wheel levels if the tick range becomes insufficient
- Per-CPU timer wheels for CPU-local timers
- `timer_create_in()` helper for relative deadlines

## Config

```c
// Number of buckets per wheel level
#define CONFIG_TIMER_WHEEL_BUCKETS 256

// Maximum timer entries (static pool sizing)
#define CONFIG_TIMER_MAX_ENTRIES 64
```

## Files

| File | Change |
|------|--------|
| `kernel/include/tori/kernel/container/intrusive_list.hpp` | New — generic IntrusiveList\<T\> |
| `kernel/include/tori/kernel/container/optional.hpp` | New — generic Optional\<T\> |
| `kernel/include/tori/kernel/container/span.hpp` | New — generic Span\<T\> |
| `kernel/include/tori/kernel/timer.hpp` | New — timer API header |
| `kernel/src/kernel/timer.cpp` | New — timer wheel implementation |
| `kernel/include/tori/kernel/log.hpp` | Add `flush_all()` for timer callback |
| `kernel/src/log/log.cpp` | Add `flush_all()`, use timer for periodic flush |
| `kernel/src/arch/x86_64/idt.cpp` | Call `timer::tick()` in vector 32 handler |
| `kernel/src/kernel/main.cpp` | Add `timer::init()` call |
| `kernel/config.h` | Add timer config constants |
| `kernel/src/arch/x86_64/interrupts_asm.S` | Remove `call log_flush` (optional cleanup) |
