/* HarisOS user program: an interactive demo for the Console window. Double click ECHO.ELF in the File Explorer, type
 * a line and press Enter: the program reads it with SYS_READ on fd 0 (it waits, without using the CPU, until Enter), prints
 * it back, and counts the lines. "quit" (or closing the window) ends it; "cat NAME" prints a file from a drive. */
#include "sys.h"

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}

static void cat(const char *name)
{
    long long fd = sys_open(name);
    if (fd < 0)
    {
        print("cannot open: ");
        print(name);
        print("\n");
        return;
    }
    char buf[256];
    long long total = 0, got;
    int shown = 0;
    while ((got = sys_read((int)fd, buf, sizeof(buf))) > 0)
    {
        total += got;
        if (shown < 256) /* only the first bytes, as text (control bytes of a binary file are shown as '?') */
        {
            sys_write(1, buf, (usize)(got < 256 - shown ? got : 256 - shown));
            shown += (int)got;
        }
    }
    print("\n(");
    print_number(total);
    print(" bytes)\n");
    sys_close((int)fd);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    print("echo: type a line and press Enter; 'quit' ends, 'cat NAME' prints a file\n");
    int lines = 0;
    for (;;)
    {
        print("> ");
        char line[128];
        long long n = sys_read(0, line, sizeof(line) - 1);
        if (n <= 0) /* the window was closed */
            break;
        if (line[n - 1] == '\n')
            n--;
        line[n] = 0;
        lines++;
        if (same(line, "quit"))
            break;
        if (starts_with(line, "cat "))
        {
            cat(line + 4);
            continue;
        }
        print("you said: ");
        print(line);
        print(" (line ");
        print_number(lines);
        print(", ");
        print_number(n);
        print(" characters)\n");
    }
    print("bye\n");
    sys_exit(lines);
}
