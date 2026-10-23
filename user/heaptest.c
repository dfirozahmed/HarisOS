/* HarisOS user program: checks SYS_BRK and the heap in libc.c (malloc, calloc, realloc, free).
 * Exits with 45 if every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

static int filled(const unsigned char *p, usize n, unsigned char v)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;

    /* the raw call */
    long long b0 = sys_brk(0);
    CHECK(1, b0 > 0 && (b0 & 4095) == 0);
    CHECK(2, sys_brk(b0 - 4096) == -1);              /* below the heap */
    CHECK(3, sys_brk(0x7FFFFFFFFFFFLL) == -1);       /* far beyond the limit */
    CHECK(4, sys_brk(-1) == -1);
    CHECK(5, sys_brk(b0 + 100) == b0 + 100 && sys_brk(0) == b0 + 100);
    volatile unsigned char *raw = (volatile unsigned char *)b0;
    raw[0] = 7;
    raw[99] = 9;
    CHECK(6, raw[0] == 7 && raw[99] == 9 && raw[50] == 0); /* new pages are zero */
    CHECK(7, sys_brk(b0) == b0);                           /* back down */
    CHECK(8, sys_brk(b0 + 5 * 4096) == b0 + 5 * 4096);
    raw[4 * 4096 + 5] = 1;                                 /* the last page is there */
    CHECK(9, raw[4 * 4096 + 5] == 1);
    CHECK(10, sys_brk(b0) == b0);
    CHECK(11, sbrk(0) == (void *)b0);

    /* malloc basics */
    unsigned char *a = malloc(100);
    unsigned char *b = malloc(200);
    unsigned char *c = malloc(1);
    CHECK(12, a && b && c && a != b && b != c);
    CHECK(13, ((usize)a & 15) == 0 && ((usize)b & 15) == 0 && ((usize)c & 15) == 0);
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    c[0] = 0xCC;
    CHECK(14, filled(a, 100, 0xAA) && filled(b, 200, 0xBB) && c[0] == 0xCC); /* no overlap */
    CHECK(15, (usize)(b > a ? b - a : a - b) >= 100);

    /* free and reuse: the freed hole is used again */
    free(b);
    unsigned char *b2 = malloc(150);
    CHECK(16, b2 == b);
    CHECK(17, filled(a, 100, 0xAA) && c[0] == 0xCC);

    /* merging: free everything, a block as big as all of it fits at the start */
    free(a);
    free(b2);
    free(c);
    unsigned char *big = malloc(300);
    CHECK(18, big == a);
    free(big);

    /* calloc, overflow, huge, zero */
    unsigned char *z = malloc(64);
    memset(z, 0xFF, 64);
    free(z);
    z = calloc(8, 8);
    CHECK(19, z && filled(z, 64, 0)); /* the same block as before, cleared */
    CHECK(20, calloc((usize)1 << 62, 8) == 0);
    CHECK(21, malloc((usize)1 << 41) == 0);
    CHECK(22, malloc(1024ULL * 1024 * 1024) == 0); /* more than the heap limit */
    void *zero = malloc(0);
    CHECK(23, zero != 0);
    free(zero);
    free(0);
    free(z);

    /* realloc keeps the data when it moves */
    unsigned char *r = malloc(32);
    memset(r, 0x5A, 32);
    unsigned char *keep = malloc(32); /* blocks r from growing in place */
    r = realloc(r, 4000);
    CHECK(24, r && filled(r, 32, 0x5A));
    CHECK(25, realloc(r, 10) == r && filled(r, 10, 0x5A)); /* smaller: same block */
    CHECK(26, realloc(0, 20) != 0);
    free(keep);
    free(r);

    /* a lot of blocks, then freed in a scrambled order, and the heap is whole again */
    unsigned char *p[200];
    for (int i = 0; i < 200; i++)
    {
        p[i] = malloc((usize)(i % 37) * 7 + 1);
        if (p[i])
            memset(p[i], i, (usize)(i % 37) * 7 + 1);
    }
    int ok = 1;
    for (int i = 0; i < 200; i++)
        ok = ok && p[i] && filled(p[i], (usize)(i % 37) * 7 + 1, (unsigned char)i);
    CHECK(27, ok);
    for (int i = 0; i < 200; i += 2)
        free(p[i]);
    for (int i = 1; i < 200; i += 2)
        free(p[i]);
    unsigned char *whole = malloc(20000); /* the 200 small blocks together are bigger: it only fits if they merged */
    CHECK(28, whole != 0 && whole <= p[0]);
    long long after_small = sys_brk(0);
    free(whole);

    /* a big request grows the heap; a request that fits does not */
    unsigned char *mb = malloc(1024 * 1024);
    CHECK(29, mb && sys_brk(0) > after_small);
    memset(mb, 0x11, 1024 * 1024);
    CHECK(30, filled(mb, 1024 * 1024, 0x11));
    long long grown = sys_brk(0);
    free(mb);
    mb = malloc(512 * 1024);
    CHECK(31, mb && sys_brk(0) == grown);
    free(mb);

    if (fail == 0)
        puts("heaptest: all checks passed");
    else
        printf("heaptest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 45 : fail);
}
