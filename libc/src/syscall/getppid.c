#include <unistd.h>
#include <tori/syscall.h>

pid_t getppid(void) {
    pid_t ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_getppid)
        : "rcx", "r11", "memory"
    );
    return ret;
}
