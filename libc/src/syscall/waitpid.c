#include <unistd.h>
#include <tori/syscall.h>

pid_t waitpid(pid_t pid, int* status, int options) {
    pid_t ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_waitpid), "D"(pid), "S"(status), "d"(options)
        : "rcx", "r11", "memory"
    );
    return ret;
}
