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

    // The generated ISR stubs normalize the frame by pushing:
    //   vector, error_code
    // before jumping here. After saving 15 GPRs, the CPU-saved CS is at
    // rsp + 15*8 + 2*8 + 8 = rsp + 144.
    //
    // Kernel code expects GS_BASE to address per-CPU storage. If the
    // interrupt came from CPL3, GS_BASE is the user value and the kernel
    // per-CPU base is in IA32_KERNEL_GS_BASE, so swap before calling C++.
    xor eax, eax
    test qword ptr [rsp + 144], 3
    jz .Lkernel_gs_active
    swapgs
    mov eax, 1
.Lkernel_gs_active:
    push rax

    lea rdi, [rsp + 8]
    call handle_interrupt

    call sched_needs_preempt
    test al, al
    jz .Lno_preempt
    call sched_do_preempt
.Lno_preempt:

    call log_flush

    pop rax
    test rax, rax
    jz .Lskip_swapgs_back
    swapgs
.Lskip_swapgs_back:

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
