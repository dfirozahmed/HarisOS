/* HarisOS user program: wget HOST [PATH [PORT]]
 * Fetches PATH (default "/") from HOST over plain HTTP (port 80 unless given) and prints the reply, headers and all, to the
 * console. HOST is a name ("example.com") or a dotted number. Uses the socket calls: resolve, socket, connect, send, recv. */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    if (argc < 2)
    {
        fdprintf(2, "usage: wget HOST [PATH [PORT]]\n");
        sys_exit(2);
    }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";
    long long port = argc > 3 ? strtoll(argv[3], 0, 10) : 80;
    if (port < 1 || port > 65535)
    {
        fdprintf(2, "wget: bad port\n");
        sys_exit(2);
    }

    long long ip = sys_resolve(host);
    if (ip < 0)
    {
        fdprintf(2, "wget: cannot find '%s'\n", host);
        sys_exit(1);
    }
    long long s = sys_socket();
    if (s < 0)
    {
        fdprintf(2, "wget: no socket (no network adapter?)\n");
        sys_exit(1);
    }
    if (sys_connect((int)s, (unsigned)ip, (unsigned)port) < 0)
    {
        fdprintf(2, "wget: cannot connect to %s:%d\n", host, (int)port);
        sys_close((int)s);
        sys_exit(1);
    }

    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (n <= 0 || n >= (int)sizeof req || sys_send((int)s, req, (usize)n) < 0)
    {
        fdprintf(2, "wget: could not send the request\n");
        sys_close((int)s);
        sys_exit(1);
    }

    char buf[1024];
    long long got, total = 0;
    while ((got = sys_recv((int)s, buf, sizeof buf)) > 0)
    {
        sys_write(1, buf, (usize)got);
        total += got;
    }
    sys_close((int)s);
    if (got < 0)
    {
        fdprintf(2, "\nwget: connection broke after %d bytes\n", (int)total);
        sys_exit(1);
    }
    sys_exit(0);
}
