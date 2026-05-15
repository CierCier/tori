#include <unistd.h>
#include <tori/syscall.h>

pid_t fork(void) {
    pid_t ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_fork)
        : "rcx", "r11", "memory"
    );
    return ret;
}
