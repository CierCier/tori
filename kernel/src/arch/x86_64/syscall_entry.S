.intel_syntax noprefix
.section .text

// Syscall entry — called when userspace executes `syscall`.
//
// On entry:
//   rax = syscall number
//   rcx = user RIP (return address)
//   r11 = user RFLAGS
//   rdi, rsi, rdx, r10, r8, r9 = arguments 1-6
//   rsp = user stack
.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    swapgs
    mov gs:[8], rsp         // save user RSP
    mov rsp, gs:[0]         // load kernel stack

    // Save callee-saved registers
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

    // Save syscall arguments (as received from userspace)
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi

    // Save user RIP (rcx) and RFLAGS (r11)
    push r11
    push rcx

    // handle_syscall(uint64_t number, SyscallFrame* frame)
    mov rdi, rax             // rdi = syscall number
    mov rsi, rsp             // rsi = frame pointer
    cld
    call handle_syscall

    // Restore rcx, r11 for sysretq
    pop rcx                  // user RIP
    pop r11                  // user RFLAGS

    // Skip saved args + callee-saved registers
    add rsp, 12*8

    // Restore user RSP
    mov rsp, gs:[8]
    swapgs
    sysretq
.size syscall_entry, . - syscall_entry
