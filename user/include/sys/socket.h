#ifndef HARIS_H_SYS_SOCKET_H
#define HARIS_H_SYS_SOCKET_H 1
/* <sys/socket.h>: only the types and constants that ported code needs to compile. The real socket calls of HarisOS are sys_socket /
 * sys_connect / sys_send / sys_recv in sys.h; a POSIX layer over them comes with the browser's fetcher. */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 10
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};
#endif
