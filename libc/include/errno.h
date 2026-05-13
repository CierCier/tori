#ifndef ERRNO_H
#define ERRNO_H

#define EPERM   1
#define ENOENT  2
#define EIO     5
#define ENOMEM  12
#define EINVAL  22

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#ifdef __cplusplus
}
#endif

#endif
