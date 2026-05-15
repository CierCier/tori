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

using namespace tori::proc;

// Copy a null-terminated string from userspace into a kernel buffer.
// Returns 0 on success, -E_INVALID if the range is invalid.
int copy_string_from_user(const char* user_src, char* kernel_dst, size_t max_len) {
    if (!user_src) return -E_INVALID;
    for (size_t i = 0; i < max_len - 1; ++i) {
        char c = static_cast<const volatile char*>(user_src)[i];
        kernel_dst[i] = c;
        if (c == '\0') return 0;
    }
    kernel_dst[max_len - 1] = '\0';
    return 0;
}

// Read the current user RSP saved in per-CPU storage by syscall_entry.
uint64_t read_user_rsp() {
    uint64_t rsp;
    asm volatile("mov %%gs:8, %0" : "=r"(rsp) : : "memory");
    return rsp;
}

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

// Forward declarations for syscall handlers
uint64_t sys_write(Thread* thread, SyscallFrame* frame);
uint64_t sys_exit(Thread* thread, SyscallFrame* frame);
uint64_t sys_fork(Thread* thread, SyscallFrame* frame);
uint64_t sys_exec(Thread* thread, SyscallFrame* frame);
uint64_t sys_spawn(Thread* thread, SyscallFrame* frame);
uint64_t sys_waitpid(Thread* thread, SyscallFrame* frame);
uint64_t sys_getppid(Thread* thread, SyscallFrame* frame);

} // namespace

extern "C" uint64_t handle_syscall(uint64_t number, tori::proc::SyscallFrame* frame) {
    auto* task = tori::sched::current_task();
    if (!task || !task->thread) {
        return -E_PERM;
    }
    auto* thread = static_cast<tori::proc::Thread*>(task->thread);

    switch (number) {
    case SYS_write:   return sys_write(thread, frame);
    case SYS_exit:    return sys_exit(thread, frame);
    case SYS_fork:    return sys_fork(thread, frame);
    case SYS_exec:    return sys_exec(thread, frame);
    case SYS_spawn:   return sys_spawn(thread, frame);
    case SYS_getpid:  return thread->process ? thread->process->pid : 0;
    case SYS_waitpid: return sys_waitpid(thread, frame);
    case SYS_getppid: return sys_getppid(thread, frame);
    default:          return -E_INVALID;
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
        return -E_NO_ENTRY;
    }

    uint64_t pml4_phys = tori::memory::vmm::create_user_pml4();
    if (!pml4_phys) {
        tori::vfs::vnode_unref(file);
        return -E_NO_MEM;
    }

    Process* proc = process_create(pml4_phys);
    if (!proc) {
        tori::memory::pmm::free_page(pml4_phys);
        tori::vfs::vnode_unref(file);
        return -E_NO_MEM;
    }

    ElfLoadResult load_result;
    err = elf64_load(file, pml4_phys, &load_result);
    tori::vfs::vnode_unref(file);
    if (err < 0) {
        proc->state = ProcessState::Dead;
        return static_cast<pid_t>(err);
    }

    Thread* thread = thread_create(proc, load_result.entry, load_result.stack_top);
    if (!thread) {
        proc->state = ProcessState::Dead;
        return -E_NO_MEM;
    }

    proc->thread_list = thread;
    tori::sched::sched_enqueue(&thread->task);

    return static_cast<pid_t>(proc->pid);
}

} // namespace tori::proc

namespace {

uint64_t sys_write(Thread* thread, SyscallFrame* frame) {
    int fd = static_cast<int>(frame->rdi);
    const char* buf = reinterpret_cast<const char*>(frame->rsi);
    size_t count = static_cast<size_t>(frame->rdx);

    // Validate user pointer range
    uint64_t buf_end = reinterpret_cast<uint64_t>(buf) + count;
    if (buf_end < reinterpret_cast<uint64_t>(buf) || count == 0) {
        return -E_INVALID;
    }

    size_t written = 0;
    int vfs_err = tori::vfs::write(fd, buf, count, &written);
    if (vfs_err == tori::vfs::E_SUCCESS) {
        return static_cast<uint64_t>(written);
    }

    // Fallback for unopened fds (stdout/stderr before console vnodes exist):
    // write directly to serial.
    (void)thread;
    for (size_t i = 0; i < count; ++i) {
        tori::arch::x86_64::serial::write_char(buf[i]);
    }
    return static_cast<uint64_t>(count);
}

uint64_t sys_exit(Thread* thread, SyscallFrame* frame) {
    int status = static_cast<int>(frame->rdi);
    Process* proc = thread->process;
    if (!proc) {
        thread->task.state = tori::sched::TaskState::Dead;
        tori::sched::block();
        return 0;
    }

    proc->exit_status = status;
    proc->state = ProcessState::Zombie;

    // Reparent any remaining children to init (PID 0).
    Process* init = tori::proc::find_init_process();
    if (init && init != proc) {
        tori::proc::reparent_children(proc, init);
    }

    // Wake parent if it is blocked in waitpid.
    if (proc->parent && proc->parent->wait_task) {
        tori::sched::wake(proc->parent->wait_task);
        proc->parent->wait_task = nullptr;
    }

    thread->task.state = tori::sched::TaskState::Dead;
    tori::sched::block();

    return 0;
}

extern "C" void fork_return();

uint64_t sys_fork(Thread* thread, SyscallFrame* frame) {
    Process* parent = thread->process;
    if (!parent) return -E_PERM;

    uint64_t child_pml4 = tori::memory::vmm::clone_address_space(parent->pml4_phys);
    if (!child_pml4) return -E_NO_MEM;

    Process* child = process_create(child_pml4, parent);
    if (!child) {
        tori::memory::vmm::free_address_space(child_pml4);
        return -E_NO_MEM;
    }

    child->fd_table = parent->fd_table;

    // Use current user RSP from per-CPU save area
    uint64_t user_rsp = read_user_rsp();

    Thread* child_thread = thread_create(child, frame->user_rip, user_rsp);
    if (!child_thread) {
        child->state = ProcessState::Dead;
        tori::memory::vmm::free_address_space(child_pml4);
        return -E_NO_MEM;
    }

    // Copy parent's SyscallFrame to child's kernel stack.
    // thread_create set up a context_switch frame at the top.
    // We want to overwrite it or place the SyscallFrame below it.
    // Actually, let's set up the child's kernel stack manually.
    
    auto* child_task = &child_thread->task;
    auto* sp = reinterpret_cast<uint64_t*>(child_task->stack.top);
    
    // 1. Place SyscallFrame on the child's kernel stack
    sp -= (sizeof(SyscallFrame) / sizeof(uint64_t));
    auto* child_frame = reinterpret_cast<SyscallFrame*>(sp);
    *child_frame = *frame;
    child_frame->rax = 0; // Child returns 0
    child_frame->user_rsp = user_rsp;

    // 2. Set up context to "return" to fork_return
    *(--sp) = reinterpret_cast<uint64_t>(fork_return);
    *(--sp) = 0; // rbx
    *(--sp) = 0; // rbp
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15
    
    child_task->context = reinterpret_cast<tori::sched::CpuContext*>(sp);

    child->thread_list = child_thread;
    tori::sched::sched_enqueue(child_task);

    return static_cast<uint64_t>(child->pid);
}

uint64_t sys_exec(Thread* thread, SyscallFrame* frame) {
    Process* proc = thread->process;
    if (!proc) return -E_PERM;

    char path_buf[256];
    int err = copy_string_from_user(
        reinterpret_cast<const char*>(frame->rdi), path_buf, sizeof(path_buf));
    if (err < 0) return err;

    tori::vfs::Vnode* file = nullptr;
    err = tori::vfs::resolve(nullptr, path_buf, &file);
    if (err < 0) return -E_NO_ENTRY;

    uint64_t new_pml4 = tori::memory::vmm::create_user_pml4();
    if (!new_pml4) {
        tori::vfs::vnode_unref(file);
        return -E_NO_MEM;
    }

    ElfLoadResult load_result;
    err = elf64_load(file, new_pml4, &load_result);
    tori::vfs::vnode_unref(file);
    if (err < 0) {
        tori::memory::vmm::free_address_space(new_pml4);
        return static_cast<uint64_t>(err);
    }

    tori::memory::vmm::free_address_space(proc->pml4_phys);

    proc->pml4_phys = new_pml4;
    thread->user_rip = load_result.entry;
    thread->user_rsp = load_result.stack_top;
    thread->task.cr3 = new_pml4;

    return 0;
}

uint64_t sys_spawn(Thread* thread, SyscallFrame* frame) {
    (void)thread;
    char path_buf[256];
    int err = copy_string_from_user(
        reinterpret_cast<const char*>(frame->rdi), path_buf, sizeof(path_buf));
    if (err < 0) return static_cast<uint64_t>(err);

    pid_t pid = tori::proc::process_spawn(path_buf);
    if (pid < 0) return static_cast<uint64_t>(static_cast<int>(pid));

    return static_cast<uint64_t>(pid);
}

uint64_t sys_waitpid(Thread* thread, SyscallFrame* frame) {
    Process* proc = thread->process;
    if (!proc) return -E_PERM;

    int target_pid = static_cast<int>(frame->rdi);
    int* status_ptr = reinterpret_cast<int*>(frame->rsi);
    int options = static_cast<int>(frame->rdx);

    constexpr int WNOHANG = 1;

    for (;;) {
        Process* child = proc->child_head;
        Process* found_zombie = nullptr;

        while (child) {
            if (target_pid <= 0 || static_cast<int>(child->pid) == target_pid) {
                if (child->state == ProcessState::Zombie) {
                    found_zombie = child;
                    break;
                }
                if (target_pid > 0) break;
            }
            child = child->child_next;
        }

        if (found_zombie) {
            if (status_ptr) {
                *status_ptr = found_zombie->exit_status;
            }
            pid_t child_pid = static_cast<pid_t>(found_zombie->pid);
            reap_process(found_zombie);
            return static_cast<uint64_t>(child_pid);
        }

        bool has_children = false;
        child = proc->child_head;
        while (child) {
            if (target_pid <= 0 || static_cast<int>(child->pid) == target_pid) {
                has_children = true;
                break;
            }
            child = child->child_next;
        }

        if (!has_children) return -E_CHILD;
        if (options & WNOHANG) return 0;

        proc->wait_task = tori::sched::current_task();
        tori::sched::block();
    }
}

uint64_t sys_getppid(Thread* thread, SyscallFrame* frame) {
    (void)frame;
    if (thread->process && thread->process->parent) {
        return thread->process->parent->pid;
    }
    return 0;
}

} // namespace
