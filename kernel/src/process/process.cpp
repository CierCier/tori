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

void link_child(Process* parent, Process* child) {
    child->child_next = nullptr;
    child->child_prev = parent->child_tail;
    if (parent->child_tail) {
        parent->child_tail->child_next = child;
    } else {
        parent->child_head = child;
    }
    parent->child_tail = child;
}

void unlink_child(Process* parent, Process* child) {
    if (child->child_prev) {
        child->child_prev->child_next = child->child_next;
    } else {
        parent->child_head = child->child_next;
    }
    if (child->child_next) {
        child->child_next->child_prev = child->child_prev;
    } else {
        parent->child_tail = child->child_prev;
    }
    child->child_next = nullptr;
    child->child_prev = nullptr;
}

void unlink_global(Process* proc) {
    if (proc->prev) {
        proc->prev->next = proc->next;
    } else {
        process_list = proc->next;
    }
    if (proc->next) {
        proc->next->prev = proc->prev;
    }
    proc->next = nullptr;
    proc->prev = nullptr;
}

} // namespace

extern "C" void tori_thread_entry() {
    auto* task = tori::sched::current_task();
    if (!task || !task->thread) {
        for (;;) asm volatile("cli; hlt");
    }
    auto* thread = static_cast<tori::proc::Thread*>(task->thread);
    // Ensure GS_BASE points to per-CPU data before entering userspace,
    // so swapgs in syscall_entry works correctly.
    // Actually, in kernel, GS_BASE should point to per-CPU.
    // When we transition to user, we should ensure KERNEL_GS_BASE points to per-CPU.
    // load_tss already set KERNEL_GS_BASE, but if we're in kernel, GS_BASE might be anything.
    // Actually, we should probably set GS_BASE here to 0 for userspace.
    asm volatile("wrmsr" : : "a"(0), "d"(0), "c"(0xC0000101) : "memory");
    enter_userspace(thread->user_rip, thread->user_rsp);
}

namespace tori::proc {

void init_process_system() {
    for (size_t i = 0; i < pid_bitmap_words; ++i) {
        pid_bitmap[i] = 0;
    }
    // PID 0 is left free — it will be allocated to the first process (init).
    next_pid_hint = 0;
    process_list = nullptr;
    system_initialized = true;
    TORI_LOG_INFO("proc", "process system initialized");
}

uint64_t pid_alloc() {
    if (!system_initialized) return ~0ULL;

    for (uint64_t offset = 0; offset < max_pids; ++offset) {
        const uint64_t pid = (next_pid_hint + offset) % max_pids;
        const uint64_t idx = pid / 64;
        const uint64_t bit = pid % 64;
        if (!(pid_bitmap[idx] & (1ULL << bit))) {
            pid_bitmap[idx] |= 1ULL << bit;
            next_pid_hint = (pid + 1) % max_pids;
            return pid;
        }
    }
    return ~0ULL;
}

void pid_free(uint64_t pid) {
    if (pid >= max_pids || !system_initialized) return;
    uint64_t idx = pid / 64;
    uint64_t bit = pid % 64;
    pid_bitmap[idx] &= ~(1ULL << bit);
}

Process* find_process(uint64_t pid) {
    Process* cur = process_list;
    while (cur) {
        if (cur->pid == pid) return cur;
        cur = cur->next;
    }
    return nullptr;
}

Process* find_init_process() {
    return find_process(0);
}

Process* process_create(uintptr_t pml4_phys, Process* parent) {
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
    if (pid == ~0ULL) {
        TORI_LOG_ERROR("proc", "failed to allocate PID (system not init or out of PIDs)");
        tori::memory::kfree(proc, sizeof(Process));
        return nullptr;
    }

    proc->pid = pid;
    proc->state = ProcessState::Alive;
    proc->exit_status = 0;
    proc->pml4_phys = pml4_phys;
    proc->parent = parent;
    proc->thread_list = nullptr;
    proc->next = process_list;
    proc->prev = nullptr;

    if (parent) {
        link_child(parent, proc);
    }

    if (process_list) process_list->prev = proc;
    process_list = proc;

    TORI_LOG_VALUE(tori::log::Level::Info, "proc", "process created", pid);
    return proc;
}

void reap_process(Process* proc) {
    if (!proc || proc->state != ProcessState::Zombie) return;

    TORI_LOG_VALUE(tori::log::Level::Info, "proc", "reaping process", proc->pid);

    if (proc->parent) {
        unlink_child(proc->parent, proc);
    }
    unlink_global(proc);

    tori::memory::vmm::free_address_space(proc->pml4_phys);
    pid_free(proc->pid);
    tori::memory::kfree(proc, sizeof(Process));
}

void reparent_children(Process* dying, Process* new_parent) {
    if (!dying || !new_parent) return;

    Process* child = dying->child_head;
    while (child) {
        Process* next = child->child_next;
        unlink_child(dying, child);
        child->parent = new_parent;
        link_child(new_parent, child);
        child = next;
    }
}

void reap_zombies_of_init() {
    Process* init = find_init_process();
    if (!init) return;

    Process* child = init->child_head;
    while (child) {
        Process* next = child->child_next;
        if (child->state == ProcessState::Zombie) {
            reap_process(child);
        }
        child = next;
    }
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

    // Zero out the kernel stack to avoid leaking data or inheriting junk
    for (size_t i = 0; i < stack_size / sizeof(uint64_t); ++i) {
        reinterpret_cast<uint64_t*>(task->stack.base)[i] = 0;
    }

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
