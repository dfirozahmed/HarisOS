/* Host test for mem.c. The functions are renamed (kmemcpy ...) so they do not collide with the host C library. */
#include <stdio.h>
#define memcpy kmemcpy
#define memmove kmemmove
#define memset kmemset
#define memcmp kmemcmp
#include "mem.h"
#include "../kernel/memory/mem.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    unsigned char a[64], b[64];
    for (int i = 0; i < 64; i++)
        a[i] = (unsigned char)(i + 1);

    /* memcpy: exact bytes, guards around the destination stay untouched, returns dst, n = 0 does nothing */
    for (int i = 0; i < 64; i++)
        b[i] = 0xEE;
    CHECK(memcpy(b + 8, a, 40) == b + 8);
    int ok = 1;
    for (int i = 0; i < 64; i++)
        if (b[i] != (i >= 8 && i < 48 ? (unsigned char)(i - 8 + 1) : 0xEE))
            ok = 0;
    CHECK(ok);
    memcpy(b, a, 0);
    CHECK(b[0] == 0xEE);

    /* memset: value is taken as a byte, returns dst, edges stay untouched */
    for (int i = 0; i < 64; i++)
        b[i] = 0x11;
    CHECK(memset(b + 4, 0x1AB, 10) == b + 4); /* 0x1AB -> byte 0xAB */
    ok = 1;
    for (int i = 0; i < 64; i++)
        if (b[i] != (i >= 4 && i < 14 ? 0xAB : 0x11))
            ok = 0;
    CHECK(ok);
    memset(b, 0, 0);
    CHECK(b[0] == 0x11);

    /* memmove: overlap forward (dst < src) and backward (dst > src), and no overlap */
    for (int i = 0; i < 64; i++)
        b[i] = (unsigned char)i;
    CHECK(memmove(b, b + 5, 20) == b);
    ok = 1;
    for (int i = 0; i < 20; i++)
        if (b[i] != i + 5)
            ok = 0;
    CHECK(ok && b[20] == 20);
    for (int i = 0; i < 64; i++)
        b[i] = (unsigned char)i;
    memmove(b + 5, b, 20);
    ok = 1;
    for (int i = 0; i < 20; i++)
        if (b[5 + i] != i)
            ok = 0;
    CHECK(ok && b[4] == 4 && b[25] == 25);
    memmove(b, b, 10); /* same place: nothing changes (b[5..24] hold 0..19 after the test above) */
    CHECK(b[9] == 4 && b[5] == 0);
    for (int i = 0; i < 64; i++)
        b[i] = (unsigned char)i;
    memmove(b + 30, b, 10);
    CHECK(b[30] == 0 && b[39] == 9);

    /* memcmp: equal, first difference decides (unsigned bytes), length limits the compare */
    unsigned char x[4] = {1, 2, 3, 4}, y[4] = {1, 2, 3, 5}, z[4] = {1, 2, 0xFF, 0};
    CHECK(memcmp(x, x, 4) == 0);
    CHECK(memcmp(x, y, 4) < 0 && memcmp(y, x, 4) > 0);
    CHECK(memcmp(x, y, 3) == 0);
    CHECK(memcmp(z, x, 4) > 0); /* 0xFF is bigger than 3: bytes are unsigned */
    CHECK(memcmp(x, y, 0) == 0);

    if (fails == 0)
        printf("mem tests passed\n");
    return fails ? 1 : 0;
}
