/* memcpy / memmove / memset / memcmp for the freestanding kernel. See include/mem.h.
 *
 * Careful: the compiler turns a plain copy or fill loop into a call to memcpy / memset - inside memcpy itself that
 * would be an endless recursion. So the forward copy and the fill use the CPU's string instructions (`rep movsb`,
 * `rep stosb`; fast on modern CPUs thanks to "fast strings"), and the loops that remain read and write through
 * volatile, which the compiler may not rewrite into a call. */
#include "mem.h"

void *memcpy(void *dst, const void *src, unsigned long long n)
{
    void *ret = dst;
    /* the direction flag is clear by the ABI; rep movsb copies RCX bytes from [RSI] to [RDI] */
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, unsigned long long n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n); /* no harmful overlap: a forward copy reads every byte before it is overwritten */
    /* the destination starts inside the source: copy backwards. Not `std; rep movsb`: an interrupt arriving with the
     * direction flag set would run its handler with it set. */
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int value, unsigned long long n)
{
    void *ret = dst;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"((unsigned char)value) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, unsigned long long n)
{
    const volatile unsigned char *p = (const volatile unsigned char *)a;
    const volatile unsigned char *q = (const volatile unsigned char *)b;
    for (unsigned long long i = 0; i < n; i++)
    {
        unsigned char x = p[i], y = q[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}
