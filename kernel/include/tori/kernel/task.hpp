#pragma once

#include <stdint.h>
#include <stddef.h>
#include <config.h>

namespace tori::sched {

enum class TaskState : uint32_t {
    Ready = 0,
    Running,
    Blocked,
    Dead,
};

struct alignas(16) CpuContext {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};

struct KernelStack {
    uint8_t* base;
    uint8_t* top;
    uint64_t physical_page;
    uint64_t page_count;
};

struct Task {
    uint64_t id;
    char name[32];

    TaskState state;

    KernelStack stack;

    CpuContext* context;

    void (*entry)(void*);
    void* arg;

    Task* next;
    Task* prev;

    uint64_t creation_time;

    Task* ready_next;
    Task* ready_prev;

    void* thread;       // back-link to owning Thread (null for kernel tasks)
    uint64_t cr3;       // PML4 physical address to load on context switch
};

void init_task_system(uint32_t bsp_lapic_id);

Task* create_task(void (*entry)(void*), void* arg, const char* name);

Task* current_task();
void set_current_task(Task* task);

uint64_t task_count();

void yield();
void block();
void wake(Task* task);

[[noreturn]] void start_scheduler();

extern "C" void context_switch(CpuContext** prev, CpuContext* next, uint64_t next_cr3);
extern "C" void task_trampoline();

// Enqueue a pre-configured task into the scheduler's ready queue and task list.
// The task must have its stack and context already set up.
// Used for user threads created via thread_create.
void sched_enqueue(Task* task);

} // namespace tori::sched
