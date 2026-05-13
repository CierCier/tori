#include <unistd.h>
#include <tori/syscall.h>

int errno;

ssize_t write(int fd, const void* buf, size_t count) {
    ssize_t ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_write), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return ret;
}
