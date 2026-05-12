#pragma once

// Maximum logical processors (CPU threads) the kernel supports.
// Used to size per-CPU arrays: TSS blocks, GDT blocks, stacks, etc.
// Must be at least 1.
#define CONFIG_MAX_CPUS 1

// Size of the initial kernel stack for each CPU, in bytes.
#define CONFIG_KERNEL_STACK_SIZE 16384

// Size of the double-fault IST stack, in bytes.
#define CONFIG_DOUBLE_FAULT_STACK_SIZE 4096
