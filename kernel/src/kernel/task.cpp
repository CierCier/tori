#include <tori/kernel/task.hpp>

#include <tori/kernel/address.hpp>
#include <tori/kernel/allocator.hpp>
#include <tori/kernel/lapic.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/sync/spinlock.hpp>
#include <tori/kernel/time.hpp>

namespace {

tori::sync::Spinlock task_lock;
tori::sched::Task* current_tasks[CONFIG_MAX_CPUS] = {};
tori::sched::Task* all_tasks_head = nullptr;
tori::sched::Task* all_tasks_tail = nullptr;
uint64_t next_task_id = 1;
uint64_t total_task_count = 0;

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

void list_remove(tori::sched::Task* task) {
    if (task->prev) task->prev->next = task->next;
    if (task->next) task->next->prev = task->prev;
    if (all_tasks_head == task) all_tasks_head = task->next;
    if (all_tasks_tail == task) all_tasks_tail = task->prev;
    task->next = nullptr;
    task->prev = nullptr;
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
    auto* task = static_cast<tori::sched::Task*>(
        tori::memory::kalloc(sizeof(tori::sched::Task), alignof(tori::sched::Task)));
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
    TORI_LOG_INFO("sched", "task returned (no scheduler yet; halting)");

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

    if (bsp_lapic_id < CONFIG_MAX_CPUS) {
        current_tasks[bsp_lapic_id] = bsp;
    }

    list_add(bsp);
    ++total_task_count;

    TORI_LOG_INFO("sched", "task system initialized");
    TORI_LOG_VALUE(log::Level::Info, "sched", "bsp lapic id", bsp_lapic_id);
    TORI_LOG_VALUE(log::Level::Info, "sched", "bsp task id", bsp->id);
}

Task* create_task(void (*entry)(void*), void* arg, const char* name) {
    tori::sync::LockGuard guard(task_lock);

    auto* task = allocate_task();
    if (task == nullptr) {
        TORI_LOG_WARN("sched", "failed to allocate task struct");
        return nullptr;
    }

    const uint64_t stack_phys = tori::memory::pmm::alloc_pages(stack_pages);
    if (stack_phys == tori::memory::pmm::invalid_physical_address) {
        TORI_LOG_WARN("sched", "failed to allocate task stack");
        tori::memory::kfree(task, sizeof(Task));
        return nullptr;
    }

    auto* stack_virt = static_cast<uint8_t*>(
        tori::memory::address::physical_to_virtual(stack_phys));

    task->id = next_task_id++;
    copy_name(task->name, name, sizeof(task->name));
    task->state = TaskState::Ready;
    task->stack = {
        .base = stack_virt,
        .top = stack_virt + stack_size,
        .physical_page = stack_phys,
        .page_count = stack_pages,
    };
    task->entry = entry;
    task->arg = arg;
    task->creation_time = tori::time::uptime_ms();

    setup_task_stack(task, reinterpret_cast<void*>(task_trampoline));

    list_add(task);
    ++total_task_count;

    TORI_LOG_INFO("sched", "task created");
    TORI_LOG_VALUE(log::Level::Info, "sched", "task id", task->id);
    TORI_LOG_TEXT_VALUE(log::Level::Info, "sched", "task name", task->name);

    return task;
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

} // namespace tori::sched
