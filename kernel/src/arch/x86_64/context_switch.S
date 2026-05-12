.intel_syntax noprefix
.section .text

.global context_switch
.type context_switch, @function
context_switch:
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

    mov [rdi], rsp

    mov rsp, rsi

    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15

    ret
.size context_switch, . - context_switch

.global task_trampoline
.type task_trampoline, @function
task_trampoline:
    call tori_sched_task_entry
    cli
    hlt
.size task_trampoline, . - task_trampoline
