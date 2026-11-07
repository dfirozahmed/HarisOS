#ifndef HARIS_H_NETINET_IN_H
#define HARIS_H_NETINET_IN_H 1
#define HARIS_STD_HEADERS 1
#include <sys/socket.h>
#include <stdint.h>
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr
{
    in_addr_t s_addr;
};
struct in6_addr
{
    uint8_t s6_addr[16];
};
struct sockaddr_in
{
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
struct sockaddr_in6
{
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};
#define INADDR_ANY 0u
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
/* x86 is little endian */
#define htons(x) ((uint16_t)((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF)))
#define ntohs(x) htons(x)
#define htonl(x) __builtin_bswap32((uint32_t)(x))
#define ntohl(x) htonl(x)
#endif
