/* <sys/types.h>: the POSIX scalar types. size_t and ssize_t come from libc.h. */
#define HARIS_STD_HEADERS 1
#include "../../libc.h"
#ifndef HARIS_SYS_TYPES
#define HARIS_SYS_TYPES 1
typedef long long off_t; /* 64 bit although long is 32 bit on this target (LLP64) */
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned long long ino_t;
typedef unsigned long long dev_t;
typedef unsigned int nlink_t;
#endif
