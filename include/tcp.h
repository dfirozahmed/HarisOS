/* TCP client: connect to a server, send and receive a byte stream, close. See kernel/net/tcp.c.
 *
 *   int s = tcp_connect(ip, 80, 5000);        handle (0..TCP_MAX_SOCKETS-1), or -1 if nobody answers / refuses
 *   tcp_send(s, data, len, 5000);             len on success (all of it acknowledged), -1 on error or timeout
 *   tcp_recv(s, buf, max, 5000);              bytes (>0); 0 = the server closed its side and all data was read;
 *                                             -1 = reset or timed out connection; -2 = nothing arrived in time
 *   tcp_close(s);                             sends FIN, waits briefly for the server's, frees the handle
 *
 * Polled like the rest of the network code: every call that waits runs net_poll() meanwhile. Limits (a learning
 * implementation): only connections we open, one segment in flight at a time (stop and wait), segments that arrive
 * out of order are dropped (the sender repeats them), no TIME_WAIT, no window probes. */
#ifndef HARIS_TCP_H
#define HARIS_TCP_H

#define TCP_MAX_SOCKETS 4

int tcp_connect(const unsigned char ip[4], unsigned port, unsigned timeout_ms);
int tcp_send(int s, const void *data, unsigned len, unsigned timeout_ms);
int tcp_recv(int s, void *buf, unsigned max, unsigned timeout_ms);
void tcp_close(int s);

#endif
