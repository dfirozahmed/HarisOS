/* Sockets: what a process's socket descriptor points at (the table of TCP endpoints the system calls use).
 *
 *   id = sock_create();                   a new socket (0 .. SOCK_MAX-1), or -1 (no adapter / table full)
 *   sock_ref(id); sock_unref(id);         one more / one less holder (a descriptor, a fork copy). The last one closes it:
 *                                         a connection sends FIN (and waits briefly for the peer's), a listener stops
 *                                         answering SYNs and resets what was never accepted.
 *
 * A socket is in one of four states, and each call works in some of them:
 *   new --sock_connect--> connected                      (client)
 *   new --sock_bind--> bound --sock_listen--> listening  (server); sock_accept makes a NEW connected socket
 *
 *   sock_connect(id, ip, port);           0, or -1 (nobody answers, refused, not a new socket). Waits for the handshake.
 *   sock_bind(id, port);                  0, or -1 (not new, port 0 or over 65535, port in use). Any local address.
 *   sock_listen(id);                      0, or -1 (not bound, no listener left)
 *   sock_accept(id, ip, &port);           the new connected socket's id (it has no holders yet: sock_ref it), SOCK_WOULDBLOCK
 *                                         when no connection is waiting (it never waits: the system call sleeps and asks
 *                                         again, so that a signal can interrupt it), -1 (not listening, no free socket)
 *   sock_send(id, data, len);             len, or -1. Waits until the peer acknowledged everything.
 *   sock_recv(id, buf, max);              bytes (>0); 0 = the peer closed and everything was read; -1 = error or reset;
 *                                         SOCK_WOULDBLOCK = nothing yet (never waits, like sock_accept)
 *   sock_resolve(name, ip);               1 and ip[4] for a name or a dotted number, else 0.
 *
 * Built on tcp.h; no state of its own except which TCP connection or listener belongs to which socket. */
#ifndef HARIS_SOCK_H
#define HARIS_SOCK_H

#define SOCK_MAX 8 /* the same as TCP_MAX_SOCKETS: every connected socket holds one TCP connection */
#define SOCK_WOULDBLOCK (-2)

int sock_create(void);
void sock_ref(int id);
void sock_unref(int id);
int sock_connect(int id, const unsigned char ip[4], unsigned port);
int sock_bind(int id, unsigned port);
int sock_listen(int id);
int sock_accept(int id, unsigned char ip[4], unsigned *port);
int sock_send(int id, const void *data, unsigned len);
int sock_recv(int id, void *buf, unsigned max);
int sock_resolve(const char *name, unsigned char ip[4]);

#endif
