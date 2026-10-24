/* Socket table. See include/sock.h. */
#include "net.h"
#include "sock.h"
#include "tcp.h"

typedef struct
{
    int used;
    int refs; /* descriptors (and fork copies) that name this socket */
    int tcp;  /* handle of tcp.h, -1 until connected */
} Sock;

static Sock SOCKS[SOCK_MAX];

static int valid(int id) { return id >= 0 && id < SOCK_MAX && SOCKS[id].used; }

int sock_create(void)
{
    if (!net_present())
        return -1;
    for (int i = 0; i < SOCK_MAX; i++)
        if (!SOCKS[i].used)
        {
            SOCKS[i].used = 1;
            SOCKS[i].refs = 0; /* the caller adds its descriptor with sock_ref */
            SOCKS[i].tcp = -1;
            return i;
        }
    return -1;
}

void sock_ref(int id)
{
    if (valid(id))
        SOCKS[id].refs++;
}

void sock_unref(int id)
{
    if (!valid(id))
        return;
    if (--SOCKS[id].refs > 0)
        return;
    int h = SOCKS[id].tcp;
    SOCKS[id].used = 0;
    SOCKS[id].tcp = -1;
    if (h >= 0)
        tcp_close(h); /* after the entry is free: tcp_close waits and yields, and the id must not be handed out twice */
}

int sock_connect(int id, const unsigned char ip[4], unsigned port)
{
    if (!valid(id) || SOCKS[id].tcp >= 0 || port == 0 || port > 65535)
        return -1;
    int h = tcp_connect(ip, port, 5000);
    if (h < 0 || !valid(id) || SOCKS[id].tcp >= 0) /* (the table may have changed while we waited) */
    {
        if (h >= 0)
            tcp_close(h);
        return -1;
    }
    SOCKS[id].tcp = h;
    return 0;
}

int sock_send(int id, const void *data, unsigned len)
{
    if (!valid(id) || SOCKS[id].tcp < 0)
        return -1;
    return tcp_send(SOCKS[id].tcp, data, len, 10000);
}

int sock_recv(int id, void *buf, unsigned max)
{
    if (!valid(id) || SOCKS[id].tcp < 0)
        return -1;
    net_poll(); /* let arrived segments in; tcp_recv with no timeout only looks at what is there */
    int r = tcp_recv(SOCKS[id].tcp, buf, max, 0);
    return r == -2 ? SOCK_WOULDBLOCK : r;
}

int sock_resolve(const char *name, unsigned char ip[4]) { return net_resolve(name, ip, 2000); }
