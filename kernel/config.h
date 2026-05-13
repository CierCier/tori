#pragma once

// Maximum logical processors (CPU threads) the kernel supports.
// Used to size per-CPU arrays: TSS blocks, GDT blocks, stacks, etc.
// Must be at least 1.
#define CONFIG_MAX_CPUS 128

// Size of the initial kernel stack for each CPU, in bytes.
#define CONFIG_KERNEL_STACK_SIZE 16384

// Size of the double-fault IST stack, in bytes.
#define CONFIG_DOUBLE_FAULT_STACK_SIZE 4096

// Number of log entries in each per-CPU ring buffer.
#define CONFIG_LOG_RING_ENTRIES 256

// Number of log entries to flush to sinks in a single batch during timer ISR.
#define CONFIG_LOG_FLUSH_BATCH 4

// Number of log entries in the global boot buffer used before per-CPU setup.
#define CONFIG_LOG_BOOT_ENTRIES 64

// Number of buckets per level in the timer wheel.
// Must be a power of two. 256 gives 65536 tick range with two levels.
#define CONFIG_TIMER_WHEEL_BUCKETS 256

// Maximum number of concurrent timer entries (static pool).
#define CONFIG_TIMER_MAX_ENTRIES 64

// Log flush timer period in ticks (1 tick = 1ms with current setup).
#define CONFIG_LOG_FLUSH_PERIOD_TICKS 8
