/* HarisOS user program: the other end of ipctest.c, a process that shares nothing with it but names.
 *   ipcchild shm   maps the segment "ipctest.shm": checks what ipctest wrote there, and answers in it
 *   ipcchild mq    answers every message on the queue "ipctest.q" ("ping N") on the queue "ipctest.r" ("pong N"), five times */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "shm") == 0)
    {
        volatile unsigned char *m = sys_shm_map("ipctest.shm", 0, 0); /* it exists: no size, no CREATE */
        if ((long long)m == -1)
            sys_exit(10);
        if (sys_shm_size((void *)m) != 8192 || m[0] != 'P' || m[4095] != 'Q' || m[4096] != 'R')
            sys_exit(11);
        m[1] = 'C'; /* the answer: where ipctest looks */
        m[8191] = 'Z';
        sys_exit(0);
    }
    if (strcmp(mode, "mq") == 0)
    {
        long long in = sys_mq_open("ipctest.q", 0), out = sys_mq_open("ipctest.r", 0);
        if (in < 0 || out < 0)
            sys_exit(20);
        for (int i = 0; i < 5; i++)
        {
            char msg[64];
            long long n = sys_read((int)in, msg, sizeof(msg));
            if (n < 4 || memcmp(msg, "ping", 4) != 0)
                sys_exit(21);
            memcpy(msg, "pong", 4);
            if (sys_write((int)out, msg, (usize)n) != n)
                sys_exit(22);
        }
        sys_exit(0);
    }
    sys_exit(1);
}
