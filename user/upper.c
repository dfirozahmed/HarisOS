/* HarisOS user program: upper. Copies its input to its output with every letter in capitals (a filter for pipes:
 * "cat notes.txt | upper"). Ends when the input does. */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    char buf[512];
    long long got;
    while ((got = sys_read(0, buf, sizeof(buf))) > 0)
    {
        for (long long i = 0; i < got; i++)
            buf[i] = (char)toupper(buf[i]);
        if (sys_write(1, buf, (usize)got) != got)
            sys_exit(1); /* nobody reads the output any more */
    }
    sys_exit(got < 0 ? 1 : 0);
}
