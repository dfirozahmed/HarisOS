/* The four memory functions the C compiler may call by itself (copying a struct, "= {0}", a fill or compare loop).
 * This kernel has no C library, so kernel/memory/mem.c provides them. Kernel code may also call them directly.
 *
 * Size is `unsigned long long` (the size_t of this x86-64 target). */
#ifndef HARIS_MEM_H
#define HARIS_MEM_H

void *memcpy(void *dst, const void *src, unsigned long long n);  /* ranges must not overlap */
void *memmove(void *dst, const void *src, unsigned long long n); /* overlap is fine */
void *memset(void *dst, int value, unsigned long long n);
int memcmp(const void *a, const void *b, unsigned long long n);  /* <0, 0, >0 by the first differing byte */

#endif
