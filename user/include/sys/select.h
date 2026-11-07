#ifndef HARIS_H_SYS_SELECT_H
#define HARIS_H_SYS_SELECT_H 1
/* <sys/select.h>: fd_set and its macros. select() itself only waits out its timeout: HarisOS has no poll on descriptors yet. */
#define HARIS_STD_HEADERS 1
#include <sys/time.h>
#define FD_SETSIZE 64
typedef struct
{
    unsigned long long bits;
} fd_set;
#define FD_ZERO(s) ((s)->bits = 0)
#define FD_SET(fd, s) ((s)->bits |= 1ull << (fd))
#define FD_CLR(fd, s) ((s)->bits &= ~(1ull << (fd)))
#define FD_ISSET(fd, s) (((s)->bits >> (fd)) & 1)
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);
#endif
