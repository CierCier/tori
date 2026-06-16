#include <tori/syscall.h>
#include <tori/types.h>
#include <errno.h>

ssize_t read(int fd, void* buf, size_t count) {
    ssize_t ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_read), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}
