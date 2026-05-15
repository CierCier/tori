#include <unistd.h>
#include <tori/syscall.h>

pid_t getpid(void) {
    pid_t ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_getpid)
        : "rcx", "r11", "memory"
    );
    return ret;
}
