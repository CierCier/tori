#include <tori/kernel/process/process.hpp>
#include <tori/kernel/process/thread.hpp>

#include <tori/kernel/allocator.hpp>
#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/vmm.hpp>

namespace {

using namespace tori::proc;

constexpr uint64_t max_pids = 32768;
constexpr uint64_t pid_bitmap_words = max_pids / 64;
uint64_t pid_bitmap[pid_bitmap_words] = {};
uint64_t next_pid_hint = 0;

tori::proc::Process* process_list = nullptr;
bool system_initialized = false;

extern "C" void thread_trampoline();
extern "C" void enter_userspace(uintptr_t entry, uintptr_t stack_top);

} // namespace

extern "C" void tori_thread_entry() {
    auto* task = tori::sched::current_task();
    if (!task || !task->thread) {
        for (;;) asm volatile("cli; hlt");
    }
    auto* thread = static_cast<tori::proc::Thread*>(task->thread);
    // Set GS base to 0 for userspace before the first iretq.
    // swapgs in the syscall entry will then swap in the kernel's per-CPU data
    // from MSR_KERNEL_GS_BASE (set by load_percpu_gs_base).
    asm volatile("wrmsr" : : "a"(0), "d"(0), "c"(0xC0000101) : "memory");
    enter_userspace(thread->user_rip, thread->user_rsp);
}

namespace tori::proc {

void init_process_system() {
    for (size_t i = 0; i < pid_bitmap_words; ++i) {
        pid_bitmap[i] = 0;
    }
    pid_bitmap[0] |= 1ULL << 0;
    next_pid_hint = 1;
    process_list = nullptr;
    system_initialized = true;
    TORI_LOG_INFO("proc", "process system initialized");
}

uint64_t pid_alloc() {
    if (!system_initialized) return 0;

    for (uint64_t w = 0; w < pid_bitmap_words; ++w) {
        uint64_t idx = (next_pid_hint + w) % pid_bitmap_words;
        if (pid_bitmap[idx] == ~0ULL) continue;
        for (uint64_t b = 0; b < 64; ++b) {
            if (!(pid_bitmap[idx] & (1ULL << b))) {
                uint64_t pid = idx * 64 + b;
                if (pid >= max_pids) return 0;
                pid_bitmap[idx] |= 1ULL << b;
                next_pid_hint = (pid + 1) % max_pids;
                return pid;
            }
        }
    }
    return 0;
}

void pid_free(uint64_t pid) {
    if (pid == 0 || pid >= max_pids || !system_initialized) return;
    uint64_t idx = pid / 64;
    uint64_t bit = pid % 64;
    pid_bitmap[idx] &= ~(1ULL << bit);
}

Process* process_create(uintptr_t pml4_phys) {
    auto* proc = static_cast<Process*>(
        tori::memory::kalloc(sizeof(Process), alignof(Process)));
    if (!proc) {
        TORI_LOG_ERROR("proc", "failed to allocate Process struct from heap");
        return nullptr;
    }

    // Zero-initialize the entire struct, including fd_table
    auto* bytes = reinterpret_cast<uint8_t*>(proc);
    for (size_t i = 0; i < sizeof(Process); ++i) {
        bytes[i] = 0;
    }

    uint64_t pid = pid_alloc();
    if (pid == 0) {
        TORI_LOG_ERROR("proc", "failed to allocate PID (system not init or out of PIDs)");
        tori::memory::kfree(proc, sizeof(Process));
        return nullptr;
    }

    proc->pid = pid;
    proc->state = ProcessState::Alive;
    proc->exit_status = 0;
    proc->pml4_phys = pml4_phys;
    proc->parent = nullptr;
    proc->thread_list = nullptr;
    proc->next = process_list;
    proc->prev = nullptr;

    if (process_list) process_list->prev = proc;
    process_list = proc;

    TORI_LOG_VALUE(tori::log::Level::Info, "proc", "process created", pid);
    return proc;
}

Thread* thread_create(Process* process, uintptr_t entry, uintptr_t stack_top) {
    auto* thread = static_cast<Thread*>(
        tori::memory::kalloc(sizeof(Thread), alignof(Thread)));
    if (!thread) return nullptr;

    constexpr uint64_t stack_pages = 4;
    constexpr uint64_t stack_size = stack_pages * 4096;

    uint64_t stack_phys = tori::memory::pmm::alloc_pages(stack_pages);
    if (stack_phys == tori::memory::pmm::invalid_physical_address) {
        tori::memory::kfree(thread, sizeof(Thread));
        return nullptr;
    }

    auto* stack_virt = static_cast<uint8_t*>(
        tori::memory::address::physical_to_virtual(stack_phys));

    thread->process = process;
    thread->user_rip = entry;
    thread->user_rsp = stack_top;
    thread->user_rsp0 = reinterpret_cast<uint64_t>(stack_virt + stack_size);
    thread->next = nullptr;
    thread->prev = nullptr;

    auto* task = &thread->task;
    task->id = 0;
    task->state = tori::sched::TaskState::Ready;
    task->stack = {
        .base = stack_virt,
        .top = stack_virt + stack_size,
        .physical_page = stack_phys,
        .page_count = stack_pages,
    };
    task->entry = nullptr;
    task->arg = nullptr;
    task->thread = thread;
    task->cr3 = process ? process->pml4_phys : tori::memory::vmm::kernel_pml4();

    // Set up initial context on kernel stack so context_switch
    // jumps to thread_trampoline on first schedule.
    auto* sp = reinterpret_cast<uint64_t*>(task->stack.top);
    *(--sp) = reinterpret_cast<uint64_t>(thread_trampoline);
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    task->context = reinterpret_cast<tori::sched::CpuContext*>(sp);

    return thread;
}

} // namespace tori::proc
