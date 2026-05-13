.intel_syntax noprefix
.section .text

// thread_trampoline — first entry point for user threads.
// Called by context_switch when a user thread's Task is first scheduled.
.global thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    call tori_thread_entry
    cli
    hlt
.size thread_trampoline, . - thread_trampoline

// enter_userspace(uintptr_t entry, uintptr_t stack_top)
// rdi = entry (user RIP), rsi = stack_top (user RSP)
// Called once to transition from ring 0 to ring 3 via iretq.
.global enter_userspace
.type enter_userspace, @function
enter_userspace:
    // Build iretq frame on the current (kernel) stack
    push 0x23            // SS = USER_DATA | 3
    push rsi             // user RSP
    pushf                // RFLAGS
    pop rax
    or rax, 0x200        // set IF
    push rax
    push 0x2B            // CS = USER_CODE | 3
    push rdi             // user RIP

    // Zero segment registers for user mode
    xor rax, rax
    mov ds, eax
    mov es, eax
    mov fs, eax
    mov gs, eax

    iretq
.size enter_userspace, . - enter_userspace
