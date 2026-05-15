#include <unistd.h>
#include <tori/syscall.h>

int execv(const char* path, char* const argv[]) {
    (void)argv; // We don't support argv/envp yet in kernel
    int ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_exec), "D"(path)
        : "rcx", "r11", "memory"
    );
    return ret;
}
