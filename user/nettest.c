/* HarisOS user program: checks the socket system calls. Run by the kernel at boot when there is a network adapter (output
 * goes to the serial port), or by hand. Exit code 0 = all checks passed. The last check fetches a web page and needs the
 * internet; without it that one is skipped and does not count as a failure. */
#include "libc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("nettest: FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    /* an unconnected socket can neither send nor receive; bad descriptors and types are refused */
    long long s = sys_socket();
    CHECK(s >= 3);
    char buf[64];
    CHECK(sys_write((int)s, "x", 1) == -1);
    CHECK(sys_read((int)s, buf, sizeof buf) == -1);
    CHECK(sys_connect(1, SOCK_IP(10, 0, 2, 2), 80) == -1);          /* the console is no socket */
    CHECK(sys_connect((int)s, SOCK_IP(10, 0, 2, 2), 0) == -1);      /* port 0 */
    CHECK(sys_connect((int)s, SOCK_IP(10, 0, 2, 2), 70000) == -1);  /* port too big */
    CHECK(syscall3(SYS_SOCKET, 2, 0, 0) == -1);                     /* only stream sockets */
    CHECK(sys_close((int)s) == 0);
    CHECK(sys_write((int)s, "x", 1) == -1);                         /* closed */

    /* names */
    CHECK(sys_resolve("10.1.2.3") == (long long)SOCK_IP(10, 1, 2, 3));
    CHECK(sys_resolve("255.255.255.255") == 0xFFFFFFFFLL);          /* all ones is a valid address, not an error */
    CHECK(sys_resolve("") == -1);
    CHECK(sys_resolve("not a name") == -1);
    CHECK(sys_resolve((const char *)0) == -1);                      /* a bad pointer */

    /* the table: SOCK_MAX sockets, then no more; closing gives them back */
    long long all[8];
    int n = 0;
    while (n < 8 && (all[n] = sys_socket()) >= 0)
        n++;
    CHECK(n == 4);
    for (int i = 0; i < n; i++)
        sys_close((int)all[i]);
    s = sys_socket();
    CHECK(s >= 0);
    sys_close((int)s);

    /* a port nobody listens on: the host (10.0.2.2 in QEMU's network) answers the SYN with a reset */
    s = sys_socket();
    CHECK(s >= 0 && sys_connect((int)s, SOCK_IP(10, 0, 2, 2), 1) == -1);
    sys_close((int)s);

    /* the real thing, if the internet is there */
    long long ip = sys_resolve("example.com");
    if (ip < 0)
        printf("nettest: web page skipped (no internet)\n");
    else
    {
        s = sys_socket();
        CHECK(s >= 0 && sys_connect((int)s, (unsigned)ip, 80) == 0);
        CHECK(sys_connect((int)s, (unsigned)ip, 80) == -1); /* already connected */
        const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
        CHECK(sys_send((int)s, req, sizeof req - 1) == (long long)sizeof req - 1);
        char head[16];
        long long got = 0, r;
        while (got < 9 && (r = sys_recv((int)s, head + got, sizeof head - (usize)got)) > 0)
            got += r;
        CHECK(got >= 9 && memcmp(head, "HTTP/1.", 7) == 0);
        long long total = got;
        while ((r = sys_recv((int)s, buf, sizeof buf)) > 0)
            total += r;
        CHECK(r == 0 && total > 100); /* the server closed after the page: end of input, not an error */
        CHECK(sys_recv((int)s, buf, sizeof buf) == 0); /* and it stays that way */
        sys_close((int)s);
        printf("nettest: web page %d bytes\n", (int)total);
    }

    printf(fails ? "nettest: FAILED\n" : "nettest: ok\n");
    sys_exit(fails ? 1 : 0);
}
