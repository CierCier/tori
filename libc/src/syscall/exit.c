#include <unistd.h>
#include <tori/syscall.h>

void _Exit(int status) {
    asm volatile(
        "syscall"
        :
        : "a"(SYS_exit), "D"(status)
        : "rcx", "r11", "memory"
    );
    for (;;) asm volatile("cli; hlt");
}
