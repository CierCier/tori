# Process Model and ELF64 Loader Design

Date: 2026-05-13

## Overview

This spec describes the kernel-side process model, ELF64 binary loader,
syscall path, and the minimal libc infrastructure needed to launch and
run a first userspace process (`/sys/init`) on the Tori kernel.

The design follows a vertical-slice approach: the narrowest path to ring 3
is built first, then expanded with fork(COW), exec, and proper process
lifecycle management.

## Shared ABI (`shared/include/tori/`)

Headers under `shared/include/tori/` are compiled into both the kernel and
libc. They define the kernel/userspace contract.

### Syscall numbers (`syscall.h`)

Use Linux-compatible numbering for tooling familiarity:

| Number | Name       |
|--------|------------|
| 0      | write      |
| 1      | exit       |
| 2      | fork       |
| 3      | exec       |
| 4      | spawn      |
| 5      | getpid     |

### Error codes (`errno.h`)

| Value | Name        |
|-------|-------------|
| 0     | OK          |
| 1     | EPERM       |
| 2     | ENOENT      |
| 5     | EIO         |
| 12    | ENOMEM      |
| 22    | EINVAL      |

### Types (`types.h`)

- `pid_t` (int32_t)
- `ssize_t`, `size_t` (int64_t / uint64_t)
- Max PID constant

### Syscall calling convention

Follow Linux x86_64 ABI:

- `rax` = syscall number
- `rdi, rsi, rdx, r10, r8, r9` = arguments (up to 6)
- Return value in `rax`; negative = `-errno`
- Kernel preserves all registers except `rax` and `rcx`/`r11` (clobbered by `syscall` instruction)

## libc

A minimal C library seeded from the init binary's needs. Grown incrementally.

### Directory structure

```
libc/
├── CMakeLists.txt
├── include/
│   ├── unistd.h         (write, _Exit, fork, exec, spawn, getpid)
│   ├── stdlib.h         (NULL, size_t, _Exit)
│   └── errno.h          (error constants)
├── src/
│   ├── crt0/
│   │   └── _start.S     (entry point → main)
│   └── syscall/
│       ├── write.c
│       ├── exit.c
│       ├── fork.c
│       ├── exec.c
│       ├── spawn.c
│       └── getpid.c
└── tests/
```

### Startup (`crt0/_start.S`)

```
_start:
    xor rbp, rbp          ; mark deepest frame
    mov rdi, [rsp]         ; argc
    lea rsi, [rsp+8]       ; argv
    lea rdx, [rsi+rdi*8+8] ; envp (may be NULL)
    call main
    mov rdi, rax           ; exit status
    call _Exit
```

### Syscall wrapper pattern

Each syscall wrapper is a small `.c` file. Example (write):

```c
#include <unistd.h>
ssize_t write(int fd, const void* buf, size_t count) {
    ssize_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(0), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}
```

### Build

`libc/CMakeLists.txt` generates a static library `libtori_libc.a`.
The init binary links against it.

## Userspace Init Binary

Location: `userspace/apps/init/`

```c
#include <unistd.h>
int main(int argc, char** argv, char** envp) {
    const char msg[] = "Hello from init!\n";
    write(1, msg, sizeof msg - 1);
    _Exit(0);
}
```

Built as freestanding static ELF64, copied to `iso/rootfs/sys/init`.

## Process Model

### Thread struct (new)

```cpp
struct Thread {
    Task task;             // embedded scheduler task
    Process* process;      // owning process (null for kernel threads)
    uintptr_t user_rsp;    // saved user stack pointer
    uintptr_t user_rsp0;   // kernel stack top (for TSS rsp[0])
};
```

`Thread` embeds `Task` so the scheduler sees a `Task*` and works unchanged.
The back-link lets syscall handlers reach the owning process.

### Process struct (new)

```cpp
struct Process {
    uint64_t pid;
    uintptr_t pml4_phys;              // per-process page table
    FdTable fd_table;                 // cloned on fork
    ProcessState state;               // Alive, Zombie, Dead
    int exit_status;
    Process* parent;
    ListHead children;
    ListHead threads;                 // threads belonging to this process
};
```

### Task changes

Add two fields to the existing `Task`:

```cpp
struct Task {
    // ... existing fields unchanged ...
    Thread* thread;   // back-link, null for pure kernel tasks
    void* cr3;        // PML4 physical address to load on context switch
};
```

- Kernel threads: `cr3 = kernel_pml4_phys`, `thread = null`
- User threads: `cr3 = process->pml4_phys`, `thread = &thread`

### FdTable per-process

The current global `static FdEntry fd_table[MAX_FDS]` in `vfs.cpp` becomes
per-`Process`. VFS `read/write/open/close/stat` resolve the fd table from
`current_task()->thread->process->fd_table`. Kernel tasks (thread==null)
use a separate kernel-global fd table.

### PID allocator

Simple bitmap. PID range 1–32767. PID 1 reserved for init.

## Scheduler Integration

### CR3 swap

Context switch assembly gets a third argument: the incoming task's CR3.

Current signature: `context_switch(CpuContext** prev, CpuContext* next)`

New signature: `context_switch(CpuContext** prev, CpuContext* next, uintptr_t next_cr3)`

The ASM stub loads `next_cr3` into `CR3` after saving callee-saved regs.
This ensures the new task runs in its correct address space immediately.

### Return to ring 3

New assembly function for first-time user execution:

```
enter_userspace(thread):
    ; load kernel stack for future syscalls
    ; set TSS rsp[0] = thread->user_rsp0
    ; load user CR3
    ; set up user segment registers (DS/ES/FS/GS = USER_DATA)
    ; push SS (USER_DATA|3), user_rsp, RFLAGS, user_cs, user_rip
    ; iretq
```

### TSS update

`TSS.rsp[0]` must be per-thread (each thread has its own kernel stack
for syscall entry). Currently it's per-CPU only. Change: on context
switch to a user thread, update the current CPU's TSS `rsp[0]` to
the incoming thread's kernel stack top before `iretq`.

Alternatively, the `syscall` entry assembly loads `rsp` from
`MSR_GS_BASE`-relative storage (per-CPU), so `rsp[0]` is not needed
for `syscall` path (only for interrupts from user mode).

## Syscall Entry

### MSR setup

```
IA32_EFER  (0xC0000080): SCE = 1
IA32_STAR  (0xC0000081): bits 31:0 = not used
                         bits 47:32 = KERNEL_CS (for SYSCALL -> CS)
                         bits 63:48 = USER_CS | 3 (for SYSRET -> CS)
IA32_LSTAR (0xC0000082): = &syscall_entry (virtual address)
IA32_FMASK (0xC0000084): clear IF, DF, AC on syscall entry
```

### Syscall frame

Pushed on kernel stack by the assembly entry handler:

```cpp
struct SyscallFrame {
    // Saved by syscall entry assembly
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t user_rsp;   // from rsp before swapgs save
    uint64_t user_rflags; // from r11
    uint64_t user_rip;    // from rcx
    uint64_t syscall_number; // rax
    // Arguments are in registers: rdi, rsi, rdx, r10, r8, r9
};
```

### Entry assembly

```
syscall_entry:
    swapgs
    mov gs:[PER_CPU_USER_RSP], rsp     ; save user RSP
    mov rsp, gs:[PER_CPU_KERNEL_RSP]   ; load per-CPU kernel stack
    ; Allocate stack frame space
    push rbx, rbp, r12, r13, r14, r15  ; callee-saved
    ; arguments are already in rdi, rsi, rdx, r10, r8, r9
    mov rdi, rax                         ; arg0 = syscall number
    mov rsi, rsp                         ; arg1 = SyscallFrame*
    cld
    call handle_syscall
    ; return value in rax
    pop r15, r14, r13, r12, rbp, rbx
    swapgs
    ; Restore user RSP from saved location
    mov rsp, gs:[PER_CPU_USER_RSP]
    sysretq
```

### Dispatch (C++)

```cpp
void handle_syscall(uint64_t number, SyscallFrame* frame) {
    // Validate caller is a user thread
    Thread* thread = current_task()->thread;
    if (!thread) { frame->rax = -E_PERM; return; }

    switch (number) {
    case SYS_write:  frame->rax = sys_write(thread, frame);  break;
    case SYS_exit:   sys_exit(thread, frame);                 break;
    case SYS_fork:   frame->rax = sys_fork(thread, frame);    break;
    case SYS_exec:   frame->rax = sys_exec(thread, frame);    break;
    case SYS_spawn:  frame->rax = sys_spawn(thread, frame);   break;
    case SYS_getpid: frame->rax = thread->process->pid;       break;
    default:         frame->rax = -E_INVALID;                 break;
    }
}
```

## ELF64 Loader

### Location

`kernel/src/process/elf.cpp`, `kernel/include/tori/kernel/process/elf.hpp`

### Interface

```cpp
struct ElfLoadResult {
    uintptr_t entry;
    uintptr_t stack_top;
};

int elf_load(Vnode* file, bool is_pie, uintptr_t load_bias, ElfLoadResult* out);
```

- `file` — Vnode of the ELF binary (already opened)
- `is_pie` — set for position-independent executables (`ET_DYN`)
- `load_bias` — base address to add to PIE VMAs (0 for non-PIE `ET_EXEC`)
- `out->entry` — final entry point address
- `out->stack_top` — initial RSP (top of mapped user stack)

Returns 0 on success, negative errno on error.

### Validation

- Magic: `\x7fELF`
- Class: 64-bit (2)
- Endian: little (1)
- Version: 1
- OS/ABI: any (0–255 accepted)
- Machine: x86_64 (0x3E)
- Type: EXEC (2) or DYN (3)

### Segment loading (`PT_LOAD`)

For each `PT_LOAD` program header:

1. Compute vaddr = `phdr->p_vaddr` (+ `load_bias` for PIE)
2. Compute memsz = `phdr->p_memsz`, filesz = `phdr->p_filesz`
3. Align vaddr down to page boundary
4. Align (vaddr + memsz) up to page boundary
5. For each page in range: `map_page(process_pml4, page_vaddr, pmm_alloc(), user|rw)`
   - (For writable data segments, alloc is deferred to first write — no, map with RW directly for now; COW is a fork optimization)
6. Read file content: seek to `phdr->p_offset`, read `filesz` bytes from VFS, copy to mapped pages at `vaddr + load_bias`
7. Zero-fill `.bss`: memset `(vaddr + load_bias + filesz)` to 0 for `(memsz - filesz)` bytes

### Segment permissions mapping

| phdr->flags | Page flags |
|-------------|------------|
| PF_R        | user       |
| PF_R|PF_W   | user|rw    |
| PF_R|PF_X   | user|nx    |
| PF_R|PF_W|PF_X | user|rw|nx |

### Stack setup

Map two pages at a high user address (`0x7FFFFFFFE000`), guard page below.
Write initial stack with argv/envp/auxv per SysV AMD64 ABI:

```
[stack high address]
    envp string data
    argv string data
    NULL
    auxv pairs:
        AT_PHDR (3) = phdr address
        AT_PHENT (4) = 56
        AT_PHNUM (5) = phdr count
        AT_PAGESZ (6) = 4096
        AT_ENTRY (9) = entry point
        AT_NULL (0) = 0, 0
    NULL
    environment pointers (envp[n])
    NULL
    argument pointers (argv[n])
    argc
[stack low address = initial RSP]
```

## Fork with COW

### Address space clone

`vmm_clone_address_space(src_pml4_phys) -> dst_pml4_phys`:

1. Allocate new PML4 page
2. Copy kernel-space entries (same PML4 entries at indices 256–511) from src to dst
   — kernel maps are shared, never COW'd
3. For each user-space PML4 entry (indices 0–255):
   a. Walk the 4-level page table to find leaf PTEs
   b. For each leaf PTE with `Present` and `Writable`:
      - Clear the `W` bit
      - Set a reserved bit (bit 9, the software-available bit) as `COW_PENDING`
      - Flush TLB for that page
   c. Copy the entire page table subtree to the new PML4
   d. Crucially, the pages at level 0 (4KB PTEs) are shared between parent
      and child until one writes

4. Later optimization: huge pages must be split before COW (for now,
   `map_page` already splits 2MB/1GB pages during the walk)

### COW page fault

In the page fault handler (`handle_interrupt` vector 14):

```
if fault caused by user-mode write to non-writable page:
    if PTE has COW_PENDING bit set:
        new_page = pmm_alloc()
        copy page content from faulting page
        update PTE: new page frame, set W, clear COW_PENDING
        flush TLB
        return (retry instruction)
    else:
        kill process (SIGSEGV)
```

### fork syscall implementation

1. Take process lock (prevent concurrent fork/exec)
2. `vmm_clone_address_space(current->pml4_phys) -> child_pml4`
3. Allocate new `Process` with new PID
4. Copy fd table
5. Create new `Thread` with own kernel stack (PMM pages)
6. Create new `Task` in scheduler with child's CR3
7. Set child's initial context:
   - `user_rip` = parent's current RIP (so fork returns to same instruction)
   - `user_rsp` = parent's current RSP
   - return value register = 0 in child
8. Return child PID to parent
9. Add child to scheduler ready queue

## Exec syscall

1. Copy path string from userspace (must validate user pointer)
2. Open ELF file via VFS (`resolve` + `open`)
3. Allocate fresh address space (new PML4 with kernel mappings only)
4. Load ELF into new address space
5. Free old address space (walk page tables, free physical pages)
6. Update process: `pml4_phys = new_pml4`
7. Update thread: `user_rip = entry`, `user_rsp = stack_top`
8. Close the ELF file
9. Return 0

## Spawn syscall

`fork()` + `exec()` combined in one syscall (Linux-style `clone()` with
`CLONE_VM` clear):

1. Allocate new Process + Thread + Task + PML4 (no clone needed)
2. Load ELF into the new PML4 (same as exec)
3. Copy fd table from parent
4. Set child running
5. Return child PID

Spawn is simpler than fork because there's no address space cloning step.

## Init Launch and Process Lifecycle

### Boot sequence (in `kernel_main_task`)

After all kernel subsystems init and scheduler is running:

```cpp
void kernel_main_task(void*) {
    pid_t init_pid = sys_spawn("/sys/init", NULL, NULL);
    if (init_pid < 0) {
        TORI_PANIC("init", "failed to spawn /sys/init");
    }
    for (;;) yield();
}
```

### Process states

- `Alive` — running or ready
- `Zombie` — exited, parent hasn't reaped (future `waitpid`)
- `Dead` — reaped, resources freed

For the initial milestone, `exit` cleans up the process immediately
(no zombie handling). Proper `wait`/`zombie` is a follow-up.

### Init PID 1 special role

Reserved for future: reaps orphaned children, handles `SIGCHLD`.
For now, init's `_Exit(0)` prints a log message and halts the process.
The kernel-main task continues yielding.

## Build Integration

### CMake structure

- `shared/CMakeLists.txt` — INTERFACE library `tori_shared_headers`
- `libc/CMakeLists.txt` — static library `tori_libc` from crt0 + syscall stubs
- `userspace/apps/init/CMakeLists.txt` — links `tori_libc`, result copied to `iso/rootfs/sys/init`
- `kernel/CMakeLists.txt` — new `kernel/src/process/*.cpp` source files compiled in
- `kernel/Makefile` — add `process/*.o` to the link list

### File inventory of new code

| File | Purpose |
|------|---------|
| `shared/include/tori/syscall.h` | Syscall number definitions |
| `shared/include/tori/errno.h` | Error code definitions |
| `shared/include/tori/types.h` | Shared type definitions |
| `libc/include/unistd.h` | POSIX syscall wrappers |
| `libc/include/stdlib.h` | Standard library declarations |
| `libc/include/errno.h` | Userspace errno |
| `libc/src/crt0/_start.S` | CRT0 entry point |
| `libc/src/syscall/write.c` | write() syscall wrapper |
| `libc/src/syscall/exit.c` | _Exit() syscall wrapper |
| `libc/src/syscall/fork.c` | fork() wrapper |
| `libc/src/syscall/exec.c` | exec() wrapper |
| `libc/src/syscall/spawn.c` | spawn() wrapper |
| `libc/src/syscall/getpid.c` | getpid() wrapper |
| `libc/tests/` | (future) |
| `userspace/apps/init/main.c` | Userspace init binary |
| `userspace/apps/init/CMakeLists.txt` | Init build definition |
| `kernel/include/tori/kernel/process/process.hpp` | Process struct |
| `kernel/include/tori/kernel/process/thread.hpp` | Thread struct |
| `kernel/include/tori/kernel/process/elf.hpp` | ELF loader interface |
| `kernel/include/tori/kernel/process/syscall.hpp` | Syscall dispatch header |
| `kernel/src/process/process.cpp` | Process/allocator implementation |
| `kernel/src/process/thread.cpp` | Thread lifecycle |
| `kernel/src/process/elf.cpp` | ELF64 parser and loader |
| `kernel/src/process/syscall.cpp` | Syscall dispatch + handlers |
| `kernel/src/arch/x86_64/syscall_entry.S` | Syscall assembly entry |
| `kernel/src/arch/x86_64/enter_userspace.S` | First-time ring 3 entry |

### Changes to existing files

| File | Change |
|------|--------|
| `kernel/include/tori/kernel/task.hpp` | Add `thread*`, `cr3` fields |
| `kernel/src/kernel/task.cpp` | Init `thread=null, cr3=kernel_pml4` in `create_task` |
| `kernel/src/arch/x86_64/context_switch.S` | Add CR3 swap (3rd argument) |
| `kernel/src/arch/x86_64/gdt.cpp` | Add MSR init for syscall, TSS per-thread rsp[0] |
| `kernel/src/arch/x86_64/interrupts_asm.S` | No change (syscall uses separate path) |
| `kernel/src/arch/x86_64/idt.cpp` | No change (syscall is MSI-based, not IDT-based) |
| `kernel/src/fs/vfs.cpp` | FdTable becomes per-process |
| `kernel/src/kernel/main.cpp` | Init launch after scheduler start |
| `kernel/src/memory/vmm.cpp` | Add `clone_address_space()`, `free_address_space()` |
| `kernel/include/tori/kernel/vmm.hpp` | Export new VMM functions |

## Implementation Order

Implement in dependency order, each step testable:

1. `shared/` headers — syscall.h, errno.h, types.h
2. `kernel`: Add `thread*` and `cr3` to `Task`, CR3 swap in context switch
3. `kernel`: Process + Thread structs, PID allocator
4. `kernel`: FdTable per-process refactor
5. `kernel`: ELF64 loader (testable with RamFS ELF binary)
6. `kernel`: MSR init + syscall entry assembly + dispatch
7. `kernel`: enter_userspace assembly
8. `kernel`: write + exit syscall handlers
9. `kernel`: spawn syscall handler
10. `kernel`: Init launch in kernel_main_task
11. `libc/`: crt0 + syscall stubs
12. `userspace/apps/init/`: init binary
13. Build integration: wire everything together
14. Test: QEMU boot, see "Hello from init!" on serial
15. Follow-up: fork with COW, exec, zombie/exit cleanup

## Open Questions / Future Work

- **GDT layout for SYSRET on Intel**: Intel CPUs add 16 to the SYSRET
  segment selector (SYSRET CS = STAR[63:48] + 16). The current GDT layout
  has USER_CODE at index 3-4. An Intel-compatible layout needs either a
  second GDT entry at index 5 (USER_CODE_INTEL) or AMD-only support for now.
- **Dynamic linking**: Not in scope. Init is statically linked.
- **Signals**: Not in scope. Page fault kills the process.
- **waitpid/zombie**: Not in scope for first milestone.
- **Multi-threading**: Process with multiple threads is designed but not
  fully implemented in this milestone (init has one thread).
