#ifndef UNISTD_H
#define UNISTD_H

#include <tori/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t write(int fd, const void* buf, size_t count);
void _Exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
