.intel_syntax noprefix
.section .text

.extern handle_interrupt
.extern sched_needs_preempt
.extern sched_do_preempt
.extern log_flush
.global isr_common_handler
isr_common_handler:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call handle_interrupt

    call sched_needs_preempt
    test al, al
    jz .Lno_preempt
    call sched_do_preempt
.Lno_preempt:

    call log_flush

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16
    iretq
