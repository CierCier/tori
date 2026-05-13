#pragma once

#include <stdint.h>
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
};

void init_process_system();

uint64_t pid_alloc();
void pid_free(uint64_t pid);

Process* process_create(uintptr_t pml4_phys);

// Internal kernel spawn: create a new process by loading the ELF at path.
// Returns the new PID on success, or a negative errno on failure.
int process_spawn(const char* path);

} // namespace tori::proc
