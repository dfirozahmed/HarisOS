/* HarisOS user program: reads files through the system calls open / read / seek / close.
 *
 * It opens HELLO.ELF (a name without a slash is looked up on the drives, like exec does) and checks what a file API must
 * get right: the first bytes are the ELF magic, the size from seeking to the end is plausible, reading at the end gives 0,
 * seeking back and reading again gives the same bytes, a second descriptor has its own position, closing really frees the
 * descriptor, and bad arguments (no such file, a bad pointer, a bad descriptor, a bad whence) are refused, not obeyed.
 * It exits with 43 if every check holds, otherwise with the number of the first one that did not. */
#include "sys.h"

#define CHECK(n, cond)  \
    do                  \
    {                   \
        if (!(cond) && !fail) \
            fail = (n); \
    } while (0)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;
    unsigned char a[8], b[8];

    long long fd = sys_open("HELLO.ELF");
    CHECK(1, fd >= 3);
    CHECK(2, sys_read((int)fd, a, 4) == 4);
    CHECK(3, a[0] == 0x7F && a[1] == 'E' && a[2] == 'L' && a[3] == 'F');

    long long size = sys_seek((int)fd, 0, 2); /* to the end: the position is the size */
    CHECK(4, size > 64 && size < 1000000);
    CHECK(5, sys_read((int)fd, a, 4) == 0); /* nothing left */
    CHECK(6, sys_seek((int)fd, size + 100, 0) == size + 100); /* past the end is allowed */
    CHECK(7, sys_read((int)fd, a, 4) == 0);
    CHECK(8, sys_seek((int)fd, 0, 0) == 0);
    CHECK(9, sys_read((int)fd, b, 8) == 8);
    CHECK(10, b[0] == 0x7F && b[4] == 2 && b[5] == 1); /* 64-bit, little endian */
    CHECK(11, sys_seek((int)fd, -4, 1) == 4); /* from the current position (8) back 4 */
    CHECK(12, sys_read((int)fd, a, 4) == 4 && a[0] == b[4] && a[3] == b[7]);

    /* a second descriptor on the same file has a position of its own */
    long long fd2 = sys_open("hello.elf"); /* the case of the name does not matter */
    CHECK(13, fd2 >= 3 && fd2 != fd);
    CHECK(14, sys_read((int)fd2, a, 4) == 4 && a[0] == 0x7F);
    CHECK(15, sys_seek((int)fd, 0, 1) == 8); /* ... and the first one did not move */

    /* a whole read in pieces equals the size */
    CHECK(16, sys_seek((int)fd, 0, 0) == 0);
    long long total = 0, got;
    unsigned char chunk[100];
    while ((got = sys_read((int)fd, chunk, sizeof(chunk))) > 0)
        total += got;
    CHECK(17, total == size);

    /* closing frees the descriptor; using it afterwards fails; the number can be used again */
    CHECK(18, sys_close((int)fd) == 0);
    CHECK(19, sys_read((int)fd, a, 4) == -1);
    CHECK(20, sys_close((int)fd) == -1);
    long long fd3 = sys_open("HELLO.ELF");
    CHECK(21, fd3 == fd);
    sys_close((int)fd3);
    sys_close((int)fd2);

    /* things that must be refused */
    CHECK(22, sys_open("NO-SUCH-FILE.XYZ") == -1);
    CHECK(23, sys_open((const char *)0x1000) == -1);          /* a pointer into the kernel */
    CHECK(24, sys_open((const char *)0) == -1);
    CHECK(25, sys_read(99, a, 4) == -1);
    CHECK(26, sys_read(-1, a, 4) == -1);
    CHECK(27, sys_seek(99, 0, 0) == -1);
    fd = sys_open("HELLO.ELF");
    CHECK(28, sys_seek((int)fd, 0, 7) == -1);                 /* no such whence */
    CHECK(29, sys_seek((int)fd, -1, 0) == -1);                /* before the start */
    CHECK(30, sys_seek((int)fd, 0x7FFFFFFFFFFFFFFFLL, 2) == -1); /* a huge offset must not wrap around */
    CHECK(31, sys_read((int)fd, (void *)0x1000, 4) == -1);    /* reading INTO the kernel is refused */
    CHECK(32, sys_read((int)fd, (void *)0, 4) == -1);
    CHECK(33, sys_seek((int)fd, 0, 1) == 0);                  /* the refused calls moved nothing */
    CHECK(34, sys_write(1, (const void *)0x1000, 4) == -1);
    CHECK(35, sys_write(7, "x", 1) == -1);                    /* not a console descriptor */
    CHECK(36, sys_write(1, "", 0) == 0);

    /* too many open files: all slots are taken, the next open fails, and one close makes room */
    long long fds[8];
    int opened = 0;
    for (int i = 0; i < 8; i++)
    {
        fds[i] = sys_open("HELLO.ELF");
        if (fds[i] >= 0)
            opened++;
    }
    CHECK(37, opened >= 4 && opened <= 5); /* fd was one of the slots: the others fill the rest */
    for (int i = 0; i < 8; i++)
        if (fds[i] >= 0)
            sys_close((int)fds[i]);
    CHECK(38, sys_open("HELLO.ELF") >= 3);

    /* standard input with no window: end of input at once, not a hang */
    CHECK(39, sys_read(0, a, 4) == 0);

    if (fail == 0)
        print("files: all checks passed\n");
    else
    {
        print("files: check ");
        print_number(fail);
        print(" failed\n");
    }
    sys_exit(fail == 0 ? 43 : fail);
}
