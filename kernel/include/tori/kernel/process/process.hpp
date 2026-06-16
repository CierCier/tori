#pragma once

#include <stdint.h>
#include <tori/kernel/task.hpp>
#include <tori/kernel/vfs.hpp>

namespace tori::proc {

enum class ProcessState : uint32_t {
    Alive = 0,
    Zombie,
    Dead,
};

struct Process {
    uint64_t pid;
    ProcessState state;
    int exit_status;
    uintptr_t pml4_phys;
    tori::vfs::FdTable fd_table;
    Process* parent;
    Process* next;
    Process* prev;
    void* thread_list;
    Process* child_head;
    Process* child_tail;
    Process* child_next;
    Process* child_prev;
    tori::sched::Task* wait_task;

    uint64_t brk;          // current program break
    uint64_t brk_base;     // initial program break (end of ELF BSS)
};

void init_process_system();

uint64_t pid_alloc();
void pid_free(uint64_t pid);

Process* process_create(uintptr_t pml4_phys, Process* parent = nullptr);

Process* find_process(uint64_t pid);
Process* find_init_process();

void reap_process(Process* proc);
void reparent_children(Process* dying, Process* new_parent);
void reap_zombies_of_init();

int process_spawn(const char* path);

} // namespace tori::proc
