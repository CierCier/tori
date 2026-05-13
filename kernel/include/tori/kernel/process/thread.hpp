#pragma once

#include <stdint.h>
#include <tori/kernel/task.hpp>

namespace tori::proc {

struct Process;

struct Thread {
    tori::sched::Task task;
    Process* process;
    uintptr_t user_rsp;
    uintptr_t user_rip;
    uint64_t user_rsp0;
    Thread* next;
    Thread* prev;
};

Thread* thread_create(Process* process, uintptr_t entry, uintptr_t stack_top);

} // namespace tori::proc
