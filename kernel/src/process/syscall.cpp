#include <tori/kernel/process/syscall.hpp>
#include <tori/kernel/process/process.hpp>
#include <tori/kernel/process/thread.hpp>
#include <tori/kernel/process/elf.hpp>

#include <tori/kernel/gdt.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/vmm.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/vfs.hpp>
#include <tori/kernel/task.hpp>
#include "../arch/x86_64/serial.hpp"
#include <tori/syscall.h>
#include <tori/errno.h>
#include <tori/types.h>

namespace {

inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = static_cast<uint32_t>(value);
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr) : "memory");
}

using namespace tori::proc;

// Forward declarations for syscall handlers
uint64_t sys_write(Thread* thread, SyscallFrame* frame);
uint64_t sys_exit(Thread* thread, SyscallFrame* frame);
uint64_t sys_fork(Thread* thread, SyscallFrame* frame);
uint64_t sys_exec(Thread* thread, SyscallFrame* frame);
uint64_t sys_spawn(Thread* thread, SyscallFrame* frame);
uint64_t sys_getpid(Thread* thread, SyscallFrame* frame);

} // namespace

extern "C" uint64_t handle_syscall(uint64_t number, SyscallFrame* frame) {
    auto* task = tori::sched::current_task();
    if (!task || !task->thread) {
        return -E_PERM;
    }
    auto* thread = static_cast<Thread*>(task->thread);

    switch (number) {
    case SYS_write:  return sys_write(thread, frame);
    case SYS_exit:   return sys_exit(thread, frame);
    case SYS_fork:   return sys_fork(thread, frame);
    case SYS_exec:   return sys_exec(thread, frame);
    case SYS_spawn:  return sys_spawn(thread, frame);
    case SYS_getpid: return thread->process ? thread->process->pid : 0;
    default:         return -E_INVALID;
    }
}

namespace tori::proc {

extern "C" void syscall_entry();

void init_syscall() {
    uint64_t star = (static_cast<uint64_t>(0x08) << 32)      // KERNEL_CS selector
                  | (static_cast<uint64_t>(0x18) << 48);     // USER_CODE selector (AMD SYSRET)

    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | 1);  // IA32_EFER.SCE

    wrmsr(0xC0000081, star);                                  // IA32_STAR
    wrmsr(0xC0000082, reinterpret_cast<uint64_t>(syscall_entry)); // IA32_LSTAR
    
    // FMASK bits to clear on syscall entry:
    // TF(8), IF(9), DF(10), IOPL(12:13), NT(14), AC(18), ID(21)
    wrmsr(0xC0000084, 0x25700); 

    TORI_LOG_INFO("syscall", "syscall entry initialized");
}

pid_t process_spawn(const char* path) {
    if (!path) return -E_INVALID;

    TORI_LOG_INFO("proc", "spawning process");
    TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "proc", "path", path);

    tori::vfs::Vnode* file = nullptr;
    int err = tori::vfs::resolve(nullptr, path, &file);
    if (err < 0) {
        TORI_LOG_WARN("proc", "spawn: file not found");
        TORI_LOG_TEXT_VALUE(tori::log::Level::Warn, "proc", "path", path);
        return -E_NO_ENTRY;
    }

    uint64_t pml4_phys = tori::memory::vmm::create_user_pml4();
    if (!pml4_phys) {
        TORI_LOG_WARN("proc", "spawn: failed to create user PML4");
        tori::vfs::vnode_unref(file);
        return -E_NO_MEM;
    }

    Process* proc = process_create(pml4_phys);
    if (!proc) {
        TORI_LOG_WARN("proc", "spawn: failed to create process struct");
        tori::memory::pmm::free_page(pml4_phys);
        tori::vfs::vnode_unref(file);
        return -E_NO_MEM;
    }

    ElfLoadResult load_result;
    err = elf64_load(file, pml4_phys, &load_result);
    tori::vfs::vnode_unref(file);
    if (err < 0) {
        TORI_LOG_WARN("proc", "spawn: ELF load failed");
        TORI_LOG_VALUE(tori::log::Level::Warn, "proc", "err", static_cast<uint64_t>(-err));
        proc->state = ProcessState::Dead;
        return static_cast<pid_t>(err);
    }

    Thread* thread = thread_create(proc, load_result.entry, load_result.stack_top);
    if (!thread) {
        TORI_LOG_WARN("proc", "spawn: thread creation failed");
        proc->state = ProcessState::Dead;
        return -E_NO_MEM;
    }

    proc->thread_list = thread;
    tori::sched::sched_enqueue(&thread->task);

    pid_t pid = static_cast<pid_t>(proc->pid);
    TORI_LOG_INFO("proc", "process spawned");
    TORI_LOG_VALUE(tori::log::Level::Info, "proc", "pid", static_cast<uint64_t>(pid));

    return pid;
}

} // namespace tori::proc

namespace {

uint64_t sys_write(Thread* thread, SyscallFrame* frame) {
    (void)thread;
    int fd = static_cast<int>(frame->rdi);
    const char* buf = reinterpret_cast<const char*>(frame->rsi);
    size_t count = static_cast<size_t>(frame->rdx);

    // Validate user pointer range
    uint64_t buf_end = reinterpret_cast<uint64_t>(buf) + count;
    if (buf_end < reinterpret_cast<uint64_t>(buf) || count == 0) {
        return -E_INVALID;
    }

    // For init milestone: write directly to serial
    (void)fd;
    for (size_t i = 0; i < count; ++i) {
        tori::arch::x86_64::serial::write_char(buf[i]);
    }
    return static_cast<uint64_t>(count);
}

uint64_t sys_exit(Thread* thread, SyscallFrame* frame) {
    int status = static_cast<int>(frame->rdi);
    if (thread->process) {
        thread->process->exit_status = status;
        thread->process->state = ProcessState::Dead;
    }
    thread->task.state = tori::sched::TaskState::Dead;
    tori::sched::block();

    return 0;
}

uint64_t sys_fork(Thread*, SyscallFrame*) {
    return -E_INVALID;
}

uint64_t sys_exec(Thread*, SyscallFrame*) {
    return -E_INVALID;
}

uint64_t sys_spawn(Thread*, SyscallFrame*) {
    return -E_INVALID;
}

} // namespace
