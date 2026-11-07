#ifndef HARIS_H_ARPA_INET_H
#define HARIS_H_ARPA_INET_H 1
#define HARIS_STD_HEADERS 1
#include <netinet/in.h>
int inet_pton(int af, const char *src, void *dst); /* 1 = ok, 0 = not an address */
int inet_aton(const char *s, struct in_addr *a);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
#endif
