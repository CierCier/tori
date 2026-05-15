#pragma once

#include <stdint.h>

namespace tori::proc {

struct SyscallFrame {
    uint64_t user_rip;     // rcx on entry (offset 0)
    uint64_t user_rflags;  // r11 on entry (offset 8)
    uint64_t rdi;          // arg1 (offset 16)
    uint64_t rsi;          // arg2 (offset 24)
    uint64_t rdx;          // arg3 (offset 32)
    uint64_t r10;          // arg4 (offset 40)
    uint64_t r8;           // arg5 (offset 48)
    uint64_t r9;           // arg6 (offset 56)
    uint64_t rbx;          // (offset 64)
    uint64_t rbp;          // (offset 72)
    uint64_t r12;          // (offset 80)
    uint64_t r13;          // (offset 88)
    uint64_t r14;          // (offset 96)
    uint64_t r15;          // (offset 104)
    uint64_t rax;          // syscall number / return value (offset 112)
    uint64_t user_rsp;     // (offset 120)
};

void init_syscall();

} // namespace tori::proc
