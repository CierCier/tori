#include <tori/syscall.h>
#include <tori/types.h>
#include <errno.h>

int close(int fd) {
    int ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_close), "D"(fd)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}
