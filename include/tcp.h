/* TCP client: connect to a server, send and receive a byte stream, close. See kernel/net/tcp.c.
 *
 *   int s = tcp_connect(ip, 80, 5000);        handle (0..TCP_MAX_SOCKETS-1), or -1 if nobody answers / refuses
 *   tcp_send(s, data, len, 5000);             len on success (all of it acknowledged), -1 on error or timeout
 *   tcp_recv(s, buf, max, 5000);              bytes (>0); 0 = the server closed its side and all data was read;
 *                                             -1 = reset or timed out connection; -2 = nothing arrived in time
 *   tcp_close(s);                             sends FIN, waits briefly for the server's, frees the handle
 *
 * Polled like the rest of the network code: every call that waits runs net_poll() meanwhile. Limits (a learning
 * implementation): one segment in flight at a time (stop and wait), segments that arrive
 * out of order are dropped (the sender repeats them), no TIME_WAIT, no window probes. */
#ifndef HARIS_TCP_H
#define HARIS_TCP_H

#define TCP_MAX_SOCKETS 8   /* connections of both kinds together (client and accepted) */
#define TCP_MAX_LISTENERS 4 /* ports tcp_listen can serve at once */
#define TCP_BACKLOG 4       /* connections one listener holds before tcp_accept takes them (still handshaking or waiting) */

int tcp_connect(const unsigned char ip[4], unsigned port, unsigned timeout_ms);
int tcp_send(int s, const void *data, unsigned len, unsigned timeout_ms);
int tcp_recv(int s, void *buf, unsigned max, unsigned timeout_ms);
void tcp_close(int s);

/* Server side. tcp_listen(port) starts answering SYNs to that port: a listener handle (0..TCP_MAX_LISTENERS-1), or -1 (port in use, table full).
 * tcp_accept(l) hands out the oldest connection that completed the handshake: a handle like tcp_connect gives (use tcp_send, tcp_recv, tcp_close
 * on it), -2 when none is waiting yet (it never waits), -1 for a bad listener. tcp_unlisten(l) stops listening and resets the connections that
 * were not accepted. tcp_peer(h, ip, &port) tells who is on the other end (0, or -1). tcp_port_busy(port): a listener or a connection uses it. */
int tcp_listen(unsigned port);
int tcp_accept(int l);
void tcp_unlisten(int l);
int tcp_peer(int h, unsigned char ip[4], unsigned *port);
int tcp_port_busy(unsigned port);

#endif
