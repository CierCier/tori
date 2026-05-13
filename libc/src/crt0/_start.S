.intel_syntax noprefix
.section .text

.global _start
.type _start, @function
_start:
    xor rbp, rbp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    lea rdx, [rsi + rdi * 8 + 8]
    call main
    mov rdi, rax
    call _Exit
    cli
    hlt
.size _start, . - _start
