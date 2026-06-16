#include <tori/syscall.h>
#include <tori/types.h>
#include <errno.h>

void* brk(void* addr) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_brk), "D"((long)addr)
        : "rcx", "r11", "memory");
    if (ret == -1) return (void*)-1;
    return (void*)ret;
}
