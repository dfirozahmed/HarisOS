/* HarisOS user program: the child of spawntest.c. Checks the arguments it was started with and exits with
 *   77  argc == 3, argv = {"argtest", "hello", "world wide", NULL}
 *   55  argc == 1 and argv[0] is the path it was started by ("ARGTEST.ELF"), argv[1] is NULL
 *   1   anything else */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int code = 1;
    if (argc == 3 && strcmp(argv[0], "argtest") == 0 && strcmp(argv[1], "hello") == 0 &&
        strcmp(argv[2], "world wide") == 0 && argv[3] == 0)
        code = 77;
    else if (argc == 1 && strcmp(argv[0], "ARGTEST.ELF") == 0 && argv[1] == 0)
        code = 55;
    printf("argtest: argc=%d code=%d\n", (int)argc, code);
    sys_exit(code);
}
