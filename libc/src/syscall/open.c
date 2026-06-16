#include <tori/syscall.h>
#include <tori/types.h>
#include <errno.h>

int open(const char* path, int flags, ...) {
    int ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_open), "D"(path), "S"((long)flags)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}
