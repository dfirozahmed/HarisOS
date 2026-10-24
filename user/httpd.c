/* HarisOS user program: httpd [PORT]
 * A tiny web server (one client at a time). Listens on PORT (default 8080) and answers:
 *   GET /       a greeting that shows who asked
 *   GET /quit   a goodbye, then the server ends
 *   anything else: 404
 * It prints one line per request to the console. Uses the server socket calls: socket, bind, listen, accept. */
#include "libc.h"

/* Reads until the blank line that ends the request header (or the buffer is full or the client closes). Returns the bytes read. */
static int read_request(int fd, char *buf, int cap)
{
    int n = 0;
    while (n < cap - 1)
    {
        long long r = sys_recv(fd, buf + n, (usize)(cap - 1 - n));
        if (r <= 0)
            break;
        n += (int)r;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n"))
            break;
    }
    buf[n] = 0;
    return n;
}

static void reply(int fd, int code, const char *status, const char *body)
{
    char head[160];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", code, status,
                     (int)strlen(body));
    sys_send(fd, head, (usize)n);
    sys_send(fd, body, strlen(body));
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    long long port = argc > 1 ? strtoll(argv[1], 0, 10) : 8080;
    if (port < 1 || port > 65535)
    {
        fdprintf(2, "usage: httpd [PORT]\n");
        sys_exit(2);
    }
    long long ls = sys_socket();
    if (ls < 0 || sys_bind((int)ls, (unsigned)port) < 0 || sys_listen((int)ls, 4) < 0)
    {
        fdprintf(2, "httpd: cannot listen on port %d (no adapter, or the port is in use)\n", (int)port);
        sys_exit(1);
    }
    printf("httpd: listening on port %d\n", (int)port);

    for (;;)
    {
        SockAddr peer;
        long long c = sys_accept((int)ls, &peer);
        if (c < 0)
        {
            printf("httpd: accept failed\n");
            break;
        }
        char req[512];
        int n = read_request((int)c, req, sizeof req);
        char path[96] = "";
        if (n > 0 && strncmp(req, "GET ", 4) == 0)
        {
            int i = 0;
            while (req[4 + i] && req[4 + i] != ' ' && req[4 + i] != '\r' && i < (int)sizeof path - 1)
            {
                path[i] = req[4 + i];
                i++;
            }
            path[i] = 0;
        }
        printf("httpd: %d.%d.%d.%d:%d %s\n", (int)(peer.ip >> 24), (int)((peer.ip >> 16) & 255), (int)((peer.ip >> 8) & 255),
               (int)(peer.ip & 255), (int)peer.port, path[0] ? path : "(no GET)");

        int quit = 0;
        if (strcmp(path, "/") == 0)
        {
            char body[128];
            snprintf(body, sizeof body, "Hello from HarisOS!\nYou are %d.%d.%d.%d, port %d.\n", (int)(peer.ip >> 24),
                     (int)((peer.ip >> 16) & 255), (int)((peer.ip >> 8) & 255), (int)(peer.ip & 255), (int)peer.port);
            reply((int)c, 200, "OK", body);
        }
        else if (strcmp(path, "/quit") == 0)
        {
            reply((int)c, 200, "OK", "Bye.\n");
            quit = 1;
        }
        else
            reply((int)c, 404, "Not Found", "Not found.\n");
        sys_close((int)c);
        if (quit)
            break;
    }
    sys_close((int)ls);
    printf("httpd: bye\n");
    sys_exit(0);
}
