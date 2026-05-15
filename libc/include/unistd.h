#ifndef UNISTD_H
#define UNISTD_H

#include <tori/types.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t fork(void);
int execv(const char* path, char* const argv[]);
pid_t getpid(void);
pid_t getppid(void);
pid_t waitpid(pid_t pid, int* status, int options);
ssize_t write(int fd, const void* buf, size_t count);
void _Exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
