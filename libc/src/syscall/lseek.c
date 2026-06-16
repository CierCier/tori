#include <tori/syscall.h>
#include <tori/types.h>
#include <errno.h>

off_t lseek(int fd, off_t offset, int whence) {
    off_t ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_lseek), "D"(fd), "S"(offset), "d"((long)whence)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}
