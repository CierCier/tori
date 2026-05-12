#pragma once

#include <stdint.h>

namespace tori::arch::x86_64 {

// Must match the push order in interrupts_asm.S
struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
};

// Handler called from assembly stub
extern "C" void handle_interrupt(InterruptFrame* frame);

void init_idt();
void init_pic();
void init_pit();
uint64_t timer_tick_count();

} // namespace tori::arch::x86_64
