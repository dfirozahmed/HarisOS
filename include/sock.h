/* Sockets: what a process's socket descriptor points at (the table of TCP connections the system calls use).
 *
 *   id = sock_create();                   a new, unconnected socket (0 .. SOCK_MAX-1), or -1 (no adapter / table full)
 *   sock_ref(id); sock_unref(id);         one more / one less holder (a descriptor, a fork copy). The last one closes the
 *                                         connection (FIN, waits briefly for the server's) and frees the socket.
 *   sock_connect(id, ip, port);           0, or -1 (nobody answers, refused, already connected). Waits for the handshake.
 *   sock_send(id, data, len);             len, or -1. Waits until the server acknowledged everything.
 *   sock_recv(id, buf, max);              bytes (>0); 0 = the server closed and everything was read; -1 = error or reset;
 *                                         SOCK_WOULDBLOCK = nothing yet (it never waits: the system call sleeps and asks again,
 *                                         so that a signal can interrupt it).
 *   sock_resolve(name, ip);               1 and ip[4] for a name or a dotted number, else 0.
 *
 * Built on tcp.h; no state of its own except which TCP connection belongs to which socket. */
#ifndef HARIS_SOCK_H
#define HARIS_SOCK_H

#define SOCK_MAX 4 /* the same as TCP_MAX_SOCKETS: one TCP connection each */
#define SOCK_WOULDBLOCK (-2)

int sock_create(void);
void sock_ref(int id);
void sock_unref(int id);
int sock_connect(int id, const unsigned char ip[4], unsigned port);
int sock_send(int id, const void *data, unsigned len);
int sock_recv(int id, void *buf, unsigned max);
int sock_resolve(const char *name, unsigned char ip[4]);

#endif
