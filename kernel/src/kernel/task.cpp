#include <tori/kernel/task.hpp>

#include <tori/kernel/address.hpp>
#include <tori/kernel/allocator.hpp>
#include <tori/kernel/lapic.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/sync/spinlock.hpp>
#include <tori/kernel/time.hpp>
#include <tori/kernel/vmm.hpp>

namespace {

tori::sync::Spinlock task_lock;
tori::sched::Task* current_tasks[CONFIG_MAX_CPUS] = {};
tori::sched::Task* all_tasks_head = nullptr;
tori::sched::Task* all_tasks_tail = nullptr;
uint64_t next_task_id = 1;
uint64_t total_task_count = 0;

tori::sched::Task* idle_task = nullptr;

volatile bool need_reschedule[CONFIG_MAX_CPUS] = {};
static volatile bool scheduler_active = false;

struct ReadyQueue {
    tori::sched::Task* head;
    tori::sched::Task* tail;
};

ReadyQueue global_ready_queue = {};

constexpr uint64_t stack_pages = 4;
constexpr uint64_t stack_size = stack_pages * 4096;

void copy_name(char* dest, const char* src, size_t max_len) {
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    for (size_t i = 0; i < max_len - 1; ++i) {
        dest[i] = src[i];
        if (src[i] == '\0') break;
    }
    dest[max_len - 1] = '\0';
}

void list_add(tori::sched::Task* task) {
    task->next = nullptr;
    task->prev = all_tasks_tail;
    if (all_tasks_tail) {
        all_tasks_tail->next = task;
    } else {
        all_tasks_head = task;
    }
    all_tasks_tail = task;
}

void setup_task_stack(tori::sched::Task* task, void* trampoline_addr) {
    auto* sp = reinterpret_cast<uint64_t*>(task->stack.top);

    *(--sp) = reinterpret_cast<uint64_t>(trampoline_addr);
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    task->context = reinterpret_cast<tori::sched::CpuContext*>(sp);
}

tori::sched::Task* allocate_task() {
    return static_cast<tori::sched::Task*>(
        tori::memory::kalloc(sizeof(tori::sched::Task), alignof(tori::sched::Task)));
}

void ready_queue_init(ReadyQueue* rq) {
    rq->head = nullptr;
    rq->tail = nullptr;
}

void ready_queue_push(ReadyQueue* rq, tori::sched::Task* task) {
    task->ready_next = nullptr;
    task->ready_prev = rq->tail;
    if (rq->tail) {
        rq->tail->ready_next = task;
    } else {
        rq->head = task;
    }
    rq->tail = task;
}

tori::sched::Task* ready_queue_pop(ReadyQueue* rq) {
    tori::sched::Task* task = rq->head;
    if (!task) return nullptr;

    rq->head = task->ready_next;
    if (rq->head) {
        rq->head->ready_prev = nullptr;
    } else {
        rq->tail = nullptr;
    }

    task->ready_next = nullptr;
    task->ready_prev = nullptr;
    return task;
}

void ready_queue_remove(ReadyQueue* rq, tori::sched::Task* task) {
    if (task->ready_prev) {
        task->ready_prev->ready_next = task->ready_next;
    } else {
        rq->head = task->ready_next;
    }

    if (task->ready_next) {
        task->ready_next->ready_prev = task->ready_prev;
    } else {
        rq->tail = task->ready_prev;
    }

    task->ready_next = nullptr;
    task->ready_prev = nullptr;
}

void idle_entry(void*) {
    for (;;) {
        tori::log::flush();
        asm volatile("sti; hlt" : : : "memory");
    }
}

void schedule_internal() {
    tori::sched::Task* current = tori::sched::current_task();
    tori::sched::Task* next = ready_queue_pop(&global_ready_queue);

    if (!next) {
        if (current && current->state == tori::sched::TaskState::Running) {
            return;
        }
        next = idle_task;
        if (!next) return;
    }

    if (next == current) {
        if (current) current->state = tori::sched::TaskState::Running;
        return;
    }

    next->state = tori::sched::TaskState::Running;
    tori::sched::set_current_task(next);

    if (current == nullptr) {
        void* ctx = next->context;
        uint64_t next_cr3 = next->cr3;
        asm volatile(
            "mov %0, %%cr3\n\t"
            "movq %1, %%rsp\n\t"
            "popq %%rbx\n\t"
            "popq %%rbp\n\t"
            "popq %%r12\n\t"
            "popq %%r13\n\t"
            "popq %%r14\n\t"
            "popq %%r15\n\t"
            "ret\n\t"
            :
            : "r"(next_cr3), "r"(ctx)
            : "memory"
        );
        __builtin_unreachable();
    }

    tori::sched::context_switch(&current->context, next->context, next->cr3);
}

static tori::sched::Task* create_task_internal(void (*entry)(void*), void* arg, const char* name) {
    auto* task = allocate_task();
    if (task == nullptr) {
        TORI_LOG_WARN("sched", "failed to allocate task struct");
        return nullptr;
    }

    const uint64_t stack_phys = tori::memory::pmm::alloc_pages(stack_pages);
    if (stack_phys == tori::memory::pmm::invalid_physical_address) {
        TORI_LOG_WARN("sched", "failed to allocate task stack");
        tori::memory::kfree(task, sizeof(tori::sched::Task));
        return nullptr;
    }

    auto* stack_virt = static_cast<uint8_t*>(
        tori::memory::address::physical_to_virtual(stack_phys));

    task->id = next_task_id++;
    copy_name(task->name, name, sizeof(task->name));
    task->state = tori::sched::TaskState::Ready;
    task->stack = {
        .base = stack_virt,
        .top = stack_virt + stack_size,
        .physical_page = stack_phys,
        .page_count = stack_pages,
    };
    task->entry = entry;
    task->arg = arg;
    task->creation_time = tori::time::uptime_ms();
    task->thread = nullptr;
    task->cr3 = tori::memory::vmm::kernel_pml4();

    setup_task_stack(task, reinterpret_cast<void*>(tori::sched::task_trampoline));

    ready_queue_push(&global_ready_queue, task);

    list_add(task);
    ++total_task_count;

    TORI_LOG_INFO("sched", "task created");
    TORI_LOG_VALUE(tori::log::Level::Info, "sched", "task id", task->id);
    TORI_LOG_TEXT_VALUE(tori::log::Level::Info, "sched", "task name", task->name);

    return task;
}

} // namespace

extern "C" void tori_sched_task_entry() {
    auto* task = tori::sched::current_task();
    if (task == nullptr) {
        for (;;) asm volatile("cli; hlt");
    }

    if (task->entry) {
        task->entry(task->arg);
    }

    task->state = tori::sched::TaskState::Dead;
    schedule_internal();

    for (;;) {
        asm volatile("cli; hlt");
    }
}

namespace tori::sched {

void init_task_system(uint32_t bsp_lapic_id) {
    tori::sync::LockGuard guard(task_lock);

    next_task_id = 1;
    total_task_count = 0;
    all_tasks_head = nullptr;
    all_tasks_tail = nullptr;

    for (size_t i = 0; i < CONFIG_MAX_CPUS; ++i) {
        current_tasks[i] = nullptr;
    }

    ready_queue_init(&global_ready_queue);

    auto* bsp = allocate_task();
    if (bsp == nullptr) {
        TORI_PANIC("sched", "failed to allocate BSP task");
    }

    bsp->id = next_task_id++;
    copy_name(bsp->name, "bsp-main", sizeof(bsp->name));
    bsp->state = TaskState::Running;
    bsp->stack = {};
    bsp->context = nullptr;
    bsp->entry = nullptr;
    bsp->arg = nullptr;
    bsp->creation_time = tori::time::uptime_ms();
    bsp->thread = nullptr;
    bsp->cr3 = tori::memory::vmm::kernel_pml4();

    if (bsp_lapic_id < CONFIG_MAX_CPUS) {
        current_tasks[bsp_lapic_id] = bsp;
    }

    list_add(bsp);
    ++total_task_count;

    idle_task = create_task_internal(idle_entry, nullptr, "cpu0-idle");

    TORI_LOG_INFO("sched", "task system initialized");
    TORI_LOG_VALUE(log::Level::Info, "sched", "bsp lapic id", bsp_lapic_id);
    TORI_LOG_VALUE(log::Level::Info, "sched", "bsp task id", bsp->id);
    TORI_LOG_VALUE(log::Level::Info, "sched", "idle task id", idle_task ? idle_task->id : 0);
}

Task* create_task(void (*entry)(void*), void* arg, const char* name) {
    tori::sync::LockGuard guard(task_lock);
    return create_task_internal(entry, arg, name);
}

Task* current_task() {
    const uint32_t lapic_id = tori::arch::x86_64::lapic::id();
    if (lapic_id >= CONFIG_MAX_CPUS) return nullptr;
    return current_tasks[lapic_id];
}

void set_current_task(Task* task) {
    const uint32_t lapic_id = tori::arch::x86_64::lapic::id();
    if (lapic_id < CONFIG_MAX_CPUS) {
        current_tasks[lapic_id] = task;
    }
}

uint64_t task_count() {
    tori::sync::LockGuard guard(task_lock);
    return total_task_count;
}

void yield() {
    Task* current = current_task();
    if (current) {
        ready_queue_push(&global_ready_queue, current);
    }
    schedule_internal();
}

void block() {
    Task* current = current_task();
    if (current) {
        current->state = TaskState::Blocked;
    }
    schedule_internal();
}

void wake(Task* task) {
    if (task->state == TaskState::Blocked) {
        task->state = TaskState::Ready;
        ready_queue_push(&global_ready_queue, task);
    }
}

[[noreturn]] void start_scheduler() {
    Task* next = ready_queue_pop(&global_ready_queue);
    if (!next) {
        TORI_PANIC("sched", "no tasks available to start scheduler");
    }

    scheduler_active = true;

    next->state = TaskState::Running;
    set_current_task(next);

    void* ctx = next->context;
    uint64_t next_cr3 = next->cr3;
    asm volatile(
        "mov %0, %%cr3\n\t"
        "movq %1, %%rsp\n\t"
        "popq %%rbx\n\t"
        "popq %%rbp\n\t"
        "popq %%r12\n\t"
        "popq %%r13\n\t"
        "popq %%r14\n\t"
        "popq %%r15\n\t"
        "ret\n\t"
        :
        : "r"(next_cr3), "r"(ctx)
        : "memory"
    );

    __builtin_unreachable();
}

void flag_preempt() {
    const uint32_t lapic_id = tori::arch::x86_64::lapic::id();
    if (lapic_id < CONFIG_MAX_CPUS) {
        need_reschedule[lapic_id] = true;
    }
}

extern "C" bool sched_needs_preempt() {
    const uint32_t lapic_id = tori::arch::x86_64::lapic::id();
    return lapic_id < CONFIG_MAX_CPUS && need_reschedule[lapic_id];
}

extern "C" void sched_do_preempt() {
    const uint32_t lapic_id = tori::arch::x86_64::lapic::id();
    if (lapic_id >= CONFIG_MAX_CPUS) return;
    need_reschedule[lapic_id] = false;

    if (!scheduler_active) return;

    Task* current = current_task();
    if (current == nullptr) return;

    if (current->state == TaskState::Running) {
        current->state = TaskState::Ready;
        ready_queue_push(&global_ready_queue, current);
    }

    schedule_internal();
}

void sched_enqueue(Task* task) {
    tori::sync::LockGuard guard(task_lock);

    if (task->id == 0) {
        task->id = next_task_id++;
    }

    task->state = TaskState::Ready;
    ready_queue_push(&global_ready_queue, task);
    list_add(task);
    ++total_task_count;
}

} // namespace tori::sched
