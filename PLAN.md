# Tori Kernel Plan

## Summary

Tori is a freestanding 64-bit higher-half hobby kernel for UEFI-class systems. The first boot path uses Limine on x86_64, but the kernel is designed around Tori-owned abstractions so future boot sources and CPU architectures can be added without rewriting generic kernel subsystems.

The repository is a Make-based project with distinct workspaces for the kernel, future libc, future userspace, shared ABI headers, toolchain helpers, third-party dependencies, static ISO payload files, and documentation.

Milestone 1 produced a bootable kernel that:

- is loaded by Limine as a higher-half x86_64 ELF kernel;
- converts Limine boot data into an internal `BootInfo`;
- enters generic `kernel_main`;
- initializes serial and framebuffer logging;
- parses and validates the boot memory map;
- prints useful boot diagnostics;
- reclaims usable and bootloader-reclaimable memory into a page allocator;
- initializes a slice allocator backed by page allocations;
- copies boot memory map data into allocator-owned kernel storage;
- halts cleanly or panics with visible diagnostics.

## Architecture

Tori uses a portable core with target adapters.

- `kernel/src/boot/limine` owns Limine requests, responses, and protocol validation.
- `kernel/src/boot` owns Tori's stable internal boot model.
- `kernel/src/arch/x86_64` owns CPU-specific primitives and assembly glue.
- `kernel/src/platform/uefi_pc` owns UEFI PC platform assumptions.
- `kernel/src/kernel` owns architecture-independent initialization and runtime flow.
- `kernel/src/log` owns kernel logging.
- `kernel/src/memory` owns memory map modeling and later allocators.
- `kernel/targets/x86_64-limine` owns target build glue, linker script, Limine config, boot image layout, and emulator helpers.
- `libc` is reserved for the future userspace C library and is not a kernel dependency.
- `userspace` is reserved for future applications, services, and userspace libraries.
- `shared` contains only ABI-safe declarations that intentionally cross the kernel/userspace boundary.
- `toolchain` contains reusable target and toolchain configuration when needed.
- `third_party/limine` contains the Limine dependency when it is imported.
- `third_party/limine-protocol` contains the kernel-facing Limine protocol header.
- `iso/boot` contains static boot files copied into the ISO root.
- `iso/rootfs` contains the initial root filesystem payload copied into the ISO root.

Generic kernel code must not include Limine headers, UEFI types, or x86-only helpers directly. The intended handoff is:

```cpp
extern "C" void limine_entry();

void limine_entry() {
    tori::boot::BootInfo info = tori::boot::limine::collect_boot_info();
    tori::kernel_main(info);
}
```

The exact function names may change during implementation, but the dependency direction must not: target adapters call into the generic kernel after converting external data into internal Tori types.

## Boot Model

`BootInfo` is the stable contract between boot adapters and the generic kernel. It should contain:

- boot source identifier, initially `Limine`;
- kernel physical and virtual image ranges;
- higher-half direct map base when available;
- Tori-owned memory map descriptors;
- framebuffer address, dimensions, pitch, and pixel format;
- RSDP/ACPI pointer when available;
- boot module list, even if unused in Milestone 1;
- command line string when available;
- boot diagnostics flags for missing or degraded boot data.

Milestone 1 should request and validate Limine memory map, framebuffer, HHDM, kernel address, RSDP, and modules. Missing memory map is fatal. Missing framebuffer degrades to serial-only logging when serial output is available.

## Logging

Tori treats logging as a first-class kernel subsystem.

The public logging API should support levels such as trace, debug, info, warn, error, and panic. Call sites should not know whether output is going to serial, framebuffer, a ring buffer, or future debug tools.

Implementation is staged:

1. **Phase 1: early synchronous logging**
   - heapless formatting;
   - serial sink;
   - framebuffer console sink;
   - levels, categories, source file and line;
   - panic-safe output path;
   - basic color support for framebuffer output.

2. **Phase 2: buffered logging**
   - fixed-size global ring buffer;
   - sink masks;
   - boot log replay when sinks initialize;
   - structured fields where useful without requiring heap allocation.

3. **Phase 3: scheduler-aware logging**
   - per-CPU buffers;
   - deferred flushing;
   - post-boot log retrieval through a debug shell or equivalent diagnostic interface.

Milestone 1 implements the Phase 1 foundation and designs APIs so Phases 2 and 3 do not require changing normal log call sites.

## Memory

The kernel currently has two allocation layers:

- **Page allocator:** PMM-backed 4 KiB physical page allocation, including contiguous page runs.
- **Slice allocator:** fixed-size object allocation for small kernel objects, backed by PMM pages and HHDM virtual addresses.
- **Allocation facade:** `kalloc`/`kfree`, routing small allocations to slices and larger allocations to contiguous pages.

Current memory policy:

- usable memory is allocatable;
- Limine bootloader-reclaimable memory is reclaimed;
- page zero is reserved;
- kernel image, modules, framebuffer, reserved, bad, and unknown memory stay reserved;
- ACPI reclaimable memory is released after table data is parsed into kernel-owned storage.

Deferred memory work:

- virtual memory manager;
- page table ownership and remapping policy.

## Milestone Tracker

### Milestone 1: Boot And Diagnostics

Status: **complete**

- [x] Add Make freestanding build using Clang and LLD.
- [x] Keep the root Make project split into `kernel`, `libc`, `userspace`, and `shared` workspaces.
- [x] Add Limine boot image support for QEMU/OVMF.
- [x] Build Limine from the submodule and copy required UEFI files into the staged ISO root.
- [x] Add xorriso ISO generation.
- [x] Add QEMU/OVMF run helper and Make run targets.
- [x] Add higher-half x86_64 linker layout.
- [x] Add minimal entry code and generic `kernel_main`.
- [x] Add internal `BootInfo`.
- [x] Add serial and framebuffer logging.
- [x] Parse and summarize memory map.
- [x] Halt or panic cleanly.

Acceptance criteria:

- [x] QEMU boots the kernel through Limine and OVMF.
- [x] Serial output contains a deterministic boot banner and memory summary.
- [x] Framebuffer output shows the same core diagnostics when available.
- [x] Generic kernel code does not depend on Limine headers.
- [x] Kernel code does not link against the future `libc` workspace.

### Milestone 2: Memory Foundation

Status: **complete**

- [x] Add physical page allocator based on the parsed memory map.
- [x] Reclaim usable and Limine bootloader-reclaimable pages.
- [x] Keep ACPI reclaimable memory reserved until ACPI parsing exists.
- [x] Reserve page zero.
- [x] Add contiguous page allocation and freeing.
- [x] Add HHDM physical/virtual address helpers.
- [x] Add slice allocator for small kernel objects.
- [x] Add a small kernel allocation facade.
- [x] Copy boot memory map into allocator-owned kernel storage.
- [x] Add boot-time smoke tests for page, contiguous page, and slice allocation.
- [x] Add early kernel heap for larger variable-sized allocations after the page and slice layers are stable.
- [x] Add basic virtual memory ownership model (vmem_layout.hpp with kernel image region, HHDM awareness, address space constants).
- [x] Add minimal Virtual Memory Manager (VMM) with `map_page` support.
- [x] Add testable pure logic for memory region conversion and allocation edge cases where practical. *(Deferred — nice-to-have; all functional paths verified via boot smoke tests.)*

### Milestone 3: ACPI And Platform Discovery

Status: **complete**

- [x] Validate RSDP checksum and extended checksum.
- [x] Parse XSDT with RSDT fallback.
- [x] Validate ACPI table checksums before use.
- [x] Add ACPI table lookup API.
- [x] Log core table presence: MADT/APIC, FACP, HPET when present.
- [x] Decide when ACPI reclaimable memory can safely be released.
- [x] Keep parser logic separate from platform policy.

### Milestone 4: CPU Runtime

Status: **complete**

- [x] Add GDT/IDT setup.
- [x] Add exception handlers with panic diagnostics.
- [x] Add timer support (PIT at ~1000 Hz via legacy PIC).
- [x] Add interrupt-safe logging behavior.
- [x] Move to automatic source discovery via kernel Makefile.
- [x] Generate 256 ISR stubs at build time.
- [x] Disable Local APIC (enabled by OVMF) for legacy PIC operation.
- [x] Clean idle loop with `sti; hlt` and timer interrupts active.

### Milestone 5: Extended Platform Discovery

 Status: **complete**

 - [x] Parse ACPI enough to discover core platform tables (MADT).
 - [x] Enable Limine SMP request for AP discovery.
 - [x] Implement Local APIC (LAPIC) driver.
 - [x] Transition system tick to per-CPU LAPIC Timer.
 - [x] Initialize Symmetric Multiprocessing (SMP) and boot all APs.
 - [x] Keep platform parsing isolated from generic kernel policy.
### Milestone 6: Scheduling And Kernel Services

- [x] Add task/thread representation.
  - [x] Task control block (id, state, name, kernel stack, CPU context)
  - [x] Task states: Ready, Running, Blocked, Dead
  - [x] Task creation (allocates kernel stack + initializes context frame)
  - [x] Per-CPU current task pointer (indexed by LAPIC ID)
  - [x] Global task linked list with locking
  - [x] Unique 64-bit task ID allocation
  - [x] Context switch assembly (save/restore rbx, rbp, r12-r15, RSP)
  - [x] Task trampoline for first-time execution
  - [x] BSP boot task registered at init
- [x] Add scheduler helpers (building toward round-robin).
  - [x] Ready queue with push/pop/remove operations
  - [x] schedule() core dispatch (pick next ready task, context switch)
  - [x] yield() (voluntary reschedule)
  - [x] block() / wake() (sleep and resume tasks)
  - [x] Idle task per CPU (halts when nothing else to run)
  - [x] start_scheduler() one-way boot switch from BSP to first task
  - [x] Round-robin ready queue discipline (pop head, push tail)
- [x] Add preemptive round-robin scheduler with timer ticks.
  - [x] Per-CPU need_reschedule flag set by LAPIC timer ISR
  - [x] Preemption check in ISR common handler (after handle_interrupt)
  - [x] sched_do_preempt: preempts current task, calls schedule()
  - [x] schedule_internal picks next ready task, context_switch to it
  - [x] ISR return unwinds through resumed task's ISR frame naturally
- [x] Add synchronization primitives.
  - [x] Spinlock, LockGuard, TimedSpinlock (basic busy-wait locks)
  - [x] Mutex, TimedMutex (spin-now, block-later interface)
  - [x] Semaphore, TimedSemaphore (counting semaphore with timeout)
  - [x] RWLock (shared readers / exclusive writer)
  - [x] Seqlock (sequence lock for read-mostly data)
  - [x] ConditionVariable (predicate-spin now, scheduler-block later)
  - [x] IrqSpinlock, IrqLockGuard (interrupt-safe locking)
  - [x] UniqueLock (RAII adapter for Lockable/TimedLockable)
  - [x] Atomic&lt;T&gt; (type-safe atomic wrapper)
  - [x] Integrate spinlock protection into PMM, slice allocator, heap, VMM
- Upgrade logging to per-CPU/deferred behavior.

### Milestone 6.5: Filesystem Stack

Status: **complete**

- [x] Add VFS core (mount table, fd table, vnode ops, path resolution).
- [x] Add RamFS (in-memory FS backed by PMM pages).
- [x] Add Limine boot module plumbing (collect modules into BootInfo).
- [x] Fix limine.conf MODULE → module_path syntax and HHDM address handling.
- [x] Add overlayFS (N-layer stacking with whiteout support, merged readdir).
- [x] Mount overlayFS root at boot: RamFS lower (RO) + RamFS upper (RW).
- [x] Load initramfs via Limine modules into lower RamFS.
- [x] Verify end-to-end: open/read/stat through overlayFS returns correct content.

### Milestone 7: User Boundary

Status: **complete**

- [x] Define user/kernel address split.
- [x] Add syscall or message-passing entry path.
- [x] Add first user-mode execution experiment.
- [x] Clone address space for fork with COW (vmm_clone_address_space, free_address_space).
- [x] COW page fault handling in page fault handler.
- [x] Fork syscall.
- [x] Exec syscall (address space replacement).
- [x] Spawn userspace-facing syscall dispatch.
- [x] getpid libc wrapper.
- [x] VFS-backed write (with serial fallback for unopened fds).

### Milestone 8: TBD

## Test Plan

Early verification should focus on deterministic boot feedback:

- Make build succeeds with Clang.
- Kernel links with LLD using the target linker script.
- QEMU/OVMF boots the Limine image.
- Serial log includes boot source, kernel range, HHDM base, framebuffer status, and memory map summary.
- Framebuffer console displays boot banner and diagnostics when available.
- A forced missing-framebuffer path still logs over serial.
- A forced missing-memory-map path panics before generic initialization proceeds.

Later milestones should add unit-testable pure logic for memory map conversion, logging format behavior, ring buffer behavior, and allocator invariants.

## Defaults And Assumptions

- Project and namespace name: Tori.
- First target: `x86_64 + Limine + UEFI PC`.
- Repository layout uses separate `kernel`, `libc`, `userspace`, and `shared` workspaces from the start.
- Boot protocol abstraction is required from Milestone 1.
- Future architecture ports are possible, so generic kernel code must stay architecture-neutral.
- Kernel is higher-half from the first bootable milestone.
- C++ is freestanding and restricted: no exceptions, RTTI, hosted standard library, or early heap assumptions.
- VGA text mode is not a primary output path.
- Build-time constants are centralized in `kernel/config.h` as `CONFIG_*` defines with documentation comments.
