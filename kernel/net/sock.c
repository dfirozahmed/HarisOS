/* Socket table. See include/sock.h. */
#include "net.h"
#include "sock.h"
#include "tcp.h"

typedef enum
{
    SK_NEW,
    SK_BOUND,
    SK_LISTEN,
    SK_CONN
} Kind;

typedef struct
{
    int used;
    int refs;      /* descriptors (and fork copies) that name this socket */
    Kind kind;
    unsigned port; /* SK_BOUND, SK_LISTEN: the local port */
    int tcp;       /* SK_CONN: handle of a TCP connection; SK_LISTEN: handle of the TCP listener */
} Sock;

static Sock SOCKS[SOCK_MAX];

static int valid(int id) { return id >= 0 && id < SOCK_MAX && SOCKS[id].used; }

static int alloc(Kind kind)
{
    for (int i = 0; i < SOCK_MAX; i++)
        if (!SOCKS[i].used)
        {
            SOCKS[i].used = 1;
            SOCKS[i].refs = 0; /* the caller adds its descriptor with sock_ref */
            SOCKS[i].kind = kind;
            SOCKS[i].port = 0;
            SOCKS[i].tcp = -1;
            return i;
        }
    return -1;
}

int sock_create(void)
{
    if (!net_present())
        return -1;
    return alloc(SK_NEW);
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
    Kind kind = SOCKS[id].kind;
    int h = SOCKS[id].tcp;
    SOCKS[id].used = 0;
    SOCKS[id].tcp = -1;
    /* after the entry is free: tcp_close waits and yields, and the id must not be handed out twice */
    if (kind == SK_CONN && h >= 0)
        tcp_close(h);
    else if (kind == SK_LISTEN && h >= 0)
        tcp_unlisten(h);
}

int sock_connect(int id, const unsigned char ip[4], unsigned port)
{
    if (!valid(id) || SOCKS[id].kind != SK_NEW || port == 0 || port > 65535)
        return -1;
    int h = tcp_connect(ip, port, 5000);
    if (h < 0 || !valid(id) || SOCKS[id].kind != SK_NEW) /* (the table may have changed while we waited) */
    {
        if (h >= 0)
            tcp_close(h);
        return -1;
    }
    SOCKS[id].kind = SK_CONN;
    SOCKS[id].tcp = h;
    return 0;
}

int sock_bind(int id, unsigned port)
{
    if (!valid(id) || SOCKS[id].kind != SK_NEW || port == 0 || port > 65535 || tcp_port_busy(port))
        return -1;
    for (int i = 0; i < SOCK_MAX; i++) /* another socket may have claimed it with bind but not listened yet */
        if (SOCKS[i].used && (SOCKS[i].kind == SK_BOUND || SOCKS[i].kind == SK_LISTEN) && SOCKS[i].port == port)
            return -1;
    SOCKS[id].kind = SK_BOUND;
    SOCKS[id].port = port;
    return 0;
}

int sock_listen(int id)
{
    if (!valid(id) || SOCKS[id].kind != SK_BOUND)
        return -1;
    int l = tcp_listen(SOCKS[id].port);
    if (l < 0)
        return -1;
    SOCKS[id].kind = SK_LISTEN;
    SOCKS[id].tcp = l;
    return 0;
}

int sock_accept(int id, unsigned char ip[4], unsigned *port)
{
    if (!valid(id) || SOCKS[id].kind != SK_LISTEN)
        return -1;
    net_poll(); /* handshakes make progress only when somebody polls */
    int free_slot = 0;
    for (int i = 0; i < SOCK_MAX; i++)
        free_slot |= !SOCKS[i].used;
    if (!free_slot) /* leave the connection in the queue (it stays accepted-able) until a socket is closed */
        return -1;
    int h = tcp_accept(SOCKS[id].tcp);
    if (h == -2)
        return SOCK_WOULDBLOCK;
    if (h < 0)
        return -1;
    int n = alloc(SK_CONN);
    if (n < 0) /* (cannot happen: a free slot was just seen, and nothing runs in between) */
    {
        tcp_close(h);
        return -1;
    }
    SOCKS[n].tcp = h;
    tcp_peer(h, ip, port);
    return n;
}

int sock_send(int id, const void *data, unsigned len)
{
    if (!valid(id) || SOCKS[id].kind != SK_CONN)
        return -1;
    return tcp_send(SOCKS[id].tcp, data, len, 10000);
}

int sock_recv(int id, void *buf, unsigned max)
{
    if (!valid(id) || SOCKS[id].kind != SK_CONN)
        return -1;
    net_poll(); /* let arrived segments in; tcp_recv with no timeout only looks at what is there */
    int r = tcp_recv(SOCKS[id].tcp, buf, max, 0);
    return r == -2 ? SOCK_WOULDBLOCK : r;
}

int sock_resolve(const char *name, unsigned char ip[4]) { return net_resolve(name, ip, 2000); }
