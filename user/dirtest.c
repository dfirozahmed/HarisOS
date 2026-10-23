/* HarisOS user program: checks SYS_READDIR, SYS_STAT, SYS_CHDIR, SYS_GETCWD and relative paths against the RAM Disk's demo
 * content (/RAM Disk/Documents/Projects/haris.c is 18342 bytes ...). Exits with 47 if every check holds, otherwise with
 * the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

static int cwd_is(const char *want)
{
    char buf[256];
    long long n = sys_getcwd(buf, sizeof(buf));
    return n == (long long)strlen(want) && strcmp(buf, want) == 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;
    DirEntry e;

    /* the top: the drives */
    CHECK(1, cwd_is("/"));
    CHECK(2, sys_stat("/", &e) == 0 && e.is_dir && e.name[0] == 0);
    int ram = 0, count = 0;
    for (unsigned i = 0; sys_readdir("/", i, &e) == 1; i++, count++)
        ram |= strcmp(e.name, "RAM Disk") == 0;
    CHECK(3, ram && count >= 1);

    /* stat and readdir with absolute paths */
    CHECK(4, sys_stat("/RAM Disk/Documents/Projects/haris.c", &e) == 0 && !e.is_dir && e.size == 18342 && strcmp(e.name, "haris.c") == 0);
    CHECK(5, sys_stat("/ram disk/documents", &e) == 0 && e.is_dir && e.size == 0); /* case does not matter */
    CHECK(6, sys_stat("/RAM Disk/nothing", &e) == -1);
    int entries = 0, dirs_first = 1, seen_file = 0;
    for (unsigned i = 0; sys_readdir("/RAM Disk/Documents", i, &e) == 1; i++, entries++)
    {
        if (!e.is_dir)
            seen_file = 1;
        else if (seen_file)
            dirs_first = 0; /* folders come before files */
    }
    CHECK(7, entries == 4 && dirs_first); /* Projects, learning-log.txt, notes.txt, todo.md */
    CHECK(8, sys_readdir("/RAM Disk/Documents", 0, &e) == 1 && strcmp(e.name, "Projects") == 0 && e.is_dir);
    CHECK(9, sys_readdir("/RAM Disk/Documents", 4, &e) == 0);                /* past the end */
    CHECK(10, sys_readdir("/RAM Disk/Documents", 512, &e) == -1);             /* beyond what a call can reach */
    CHECK(11, sys_readdir("/RAM Disk/readme.txt", 0, &e) == -1);              /* a file is not a folder */
    CHECK(12, sys_readdir("/RAM Disk/Documents", 0, (DirEntry *)0x1000) == -1); /* a pointer into the kernel */
    CHECK(13, sys_readdir((const char *)0x1000, 0, &e) == -1);

    /* the working folder */
    CHECK(14, sys_chdir("/RAM Disk") == 0 && cwd_is("/RAM Disk"));
    CHECK(15, sys_stat("readme.txt", &e) == 0 && e.size == 1284);             /* relative */
    CHECK(16, sys_stat("Documents/notes.txt", &e) == 0 && e.size == 612);
    CHECK(17, sys_chdir("Documents") == 0 && cwd_is("/RAM Disk/Documents"));
    CHECK(18, sys_chdir("Projects/../Projects/.") == 0 && cwd_is("/RAM Disk/Documents/Projects"));
    CHECK(19, sys_stat("../notes.txt", &e) == 0 && e.size == 612);
    long long fd = sys_open("haris.h");                                      /* open relative to the folder */
    CHECK(20, fd >= 3 && sys_seek((int)fd, 0, 2) == 2096);                    /* (a RAM Disk file has no bytes, only a size) */
    if (fd >= 0)
        sys_close((int)fd);
    CHECK(21, sys_chdir("../..") == 0 && cwd_is("/RAM Disk"));
    CHECK(22, sys_chdir("..") == 0 && cwd_is("/"));
    CHECK(23, sys_chdir("../../..") == 0 && cwd_is("/"));                     /* never above the top */

    /* a plain program name still works from any folder */
    CHECK(24, sys_chdir("/RAM Disk/Documents") == 0);
    CHECK(25, sys_stat("HELLO.ELF", &e) == 0 && !e.is_dir && e.size > 64);
    long long h = sys_open("HELLO.ELF");
    CHECK(26, h >= 3);
    if (h >= 0)
        sys_close((int)h);
    long long child = sys_spawn("HELLO.ELF", 0);
    int st = 0;
    CHECK(27, child > 0 && sys_wait(child, &st) == child && st == 42);
    CHECK(28, sys_open("notes.txt") >= 3);                                   /* but a name in the folder wins */

    /* refusals leave the folder alone */
    CHECK(29, sys_chdir("/nothing") == -1 && sys_chdir("nothing") == -1 && cwd_is("/RAM Disk/Documents"));
    CHECK(30, sys_chdir("notes.txt") == -1 && cwd_is("/RAM Disk/Documents")); /* a file is not a folder */
    CHECK(31, sys_chdir((const char *)0x1000) == -1 && cwd_is("/RAM Disk/Documents"));
    char tiny[8];
    CHECK(32, sys_getcwd(tiny, sizeof(tiny)) == -1);                          /* does not fit: 20 characters */
    CHECK(33, sys_getcwd((char *)0x1000, 64) == -1 && sys_getcwd(tiny, 0) == -1);
    sys_chdir("/");

    if (fail == 0)
        puts("dirtest: all checks passed");
    else
        printf("dirtest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 47 : fail);
}
