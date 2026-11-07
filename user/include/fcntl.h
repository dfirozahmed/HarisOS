#ifndef HARIS_H_FCNTL_H
#define HARIS_H_FCNTL_H 1
/* <fcntl.h>: open() with POSIX flags, mapped on sys_open_flags. */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define F_GETFL 3
#define F_SETFL 4
int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...); /* accepts F_GETFL / F_SETFL and does nothing (there is no non-blocking mode yet) */
#endif
