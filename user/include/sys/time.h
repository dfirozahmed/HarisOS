#ifndef HARIS_H_SYS_TIME_H
#define HARIS_H_SYS_TIME_H 1
/* <sys/time.h>: struct timeval and gettimeofday (struct timespec and clock_gettime are in libc.h / <time.h>). */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
struct timeval
{
    time_t tv_sec;
    long long tv_usec;
};
int gettimeofday(struct timeval *tv, void *tz);
#endif
