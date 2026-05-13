#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void _Exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
