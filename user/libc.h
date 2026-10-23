/* HarisOS user C library, part 1: strings, memory, numbers, printf. No heap yet (malloc comes with SYS_BRK), no files
 * beyond the raw calls in sys.h. A program includes this header and is linked with libc.c (CMake does it for every
 * program in USER_PROGRAMS). Built for the same target as sys.h: "long" is 32 bits, so use "long long" for big values. */
#ifndef HARIS_USER_LIBC_H
#define HARIS_USER_LIBC_H

#include <stdarg.h>
#include "sys.h"

void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int memcmp(const void *a, const void *b, usize n);

usize strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, usize n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, usize n); /* pads with NULs like the standard one */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);

int isdigit(int c);
int isalpha(int c);
int isspace(int c);
int toupper(int c);
int tolower(int c);

/* Text to number. Leading spaces and a sign are accepted; parsing stops at the first character that is not a digit.
 * base 0 = look at the prefix (0x = 16, 0 = 8, else 10). *end gets the stop (or s itself if there were no digits). */
long long strtoll(const char *s, char **end, int base);
int atoi(const char *s);

/* printf family. Supported: %d %i %u %x %X %o %c %s %p %%, flags '-' and '0', width, and the size letters hh h l ll z.
 * snprintf always NUL-terminates (if size > 0) and returns the length the whole text would have had. */
int vsnprintf(char *buf, usize size, const char *fmt, va_list ap);
int snprintf(char *buf, usize size, const char *fmt, ...);
int printf(const char *fmt, ...);           /* to fd 1 */
int fdprintf(int fd, const char *fmt, ...); /* to fd 1 or 2 */
int puts(const char *s);                    /* the string and a newline */
int putchar(int c);

#endif
