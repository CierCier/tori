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

    // Push user RSP and rax onto stack to complete SyscallFrame
    push gs:[8]
    push rax

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

    // Update rax in the frame so fork_return pops the return value
    mov [rsp + 112], rax

    // Fall through to restore registers

.global fork_return
.type fork_return, @function
fork_return:
    // On entry, rsp points to SyscallFrame
    // If called from handle_syscall, rax contains the return value.
    // If called for the first time in a fork child, rax should be 0.
    
    pop rcx                  // user RIP
    pop r11                  // user RFLAGS
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15
    pop rax
    pop qword ptr gs:[8]     // restore user RSP into per-CPU save area

    mov rsp, gs:[8]
    swapgs
    sysretq

.size syscall_entry, . - syscall_entry
