/* HarisOS user program: checks the user C library (libc.c): string and memory functions, number parsing and printf.
 * Exits with 44 if every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;
    char b[32];

    CHECK(1, strlen("") == 0 && strlen("abc") == 3);
    CHECK(2, strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("a", "ab") < 0);
    CHECK(3, strncmp("abcX", "abcY", 3) == 0 && strncmp("abcX", "abcY", 4) < 0 && strncmp("a", "b", 0) == 0);
    strcpy(b, "hello");
    CHECK(4, strcmp(b, "hello") == 0);
    strncpy(b, "hi", 5);
    CHECK(5, b[0] == 'h' && b[1] == 'i' && b[2] == 0 && b[4] == 0); /* padded with NULs */
    CHECK(6, strchr("a/b/c", '/') != 0 && strchr("a/b/c", '/')[1] == 'b');
    CHECK(7, strrchr("a/b/c", '/')[1] == 'c' && strchr("abc", 'z') == 0 && strchr("abc", 0) != 0);
    CHECK(8, strstr("find the needle", "the")[3] == ' ' && strstr("abc", "xyz") == 0 && strstr("abc", "")[0] == 'a');

    memset(b, 'x', 8);
    CHECK(9, b[0] == 'x' && b[7] == 'x');
    memcpy(b, "0123456789", 10);
    CHECK(10, memcmp(b, "0123456789", 10) == 0 && memcmp("a", "b", 1) < 0);
    memmove(b + 2, b, 6); /* overlapping, destination higher */
    CHECK(11, memcmp(b, "01012345", 8) == 0);
    memmove(b, b + 2, 6); /* overlapping, destination lower */
    CHECK(12, memcmp(b, "012345", 6) == 0);

    CHECK(13, atoi("123") == 123 && atoi("  -45x") == -45 && atoi("+7") == 7 && atoi("abc") == 0);
    char *end;
    CHECK(14, strtoll("0x1F", &end, 0) == 31 && *end == 0);
    CHECK(15, strtoll("077", 0, 0) == 63 && strtoll("ff", 0, 16) == 255 && strtoll("101", 0, 2) == 5);
    CHECK(16, strtoll("12ab", &end, 10) == 12 && *end == 'a');
    CHECK(17, strtoll("zzz", &end, 10) == 0 && end != 0 && *end == 'z');
    CHECK(18, toupper('a') == 'A' && tolower('Q') == 'q' && toupper('1') == '1' && isdigit('5') && !isdigit('a'));
    CHECK(19, isspace('\n') && isspace(' ') && !isspace('x') && isalpha('k') && !isalpha('3'));

    char o[64];
    CHECK(20, snprintf(o, sizeof(o), "%d|%i|%u", -12, 34, 56u) == 9 && strcmp(o, "-12|34|56") == 0);
    snprintf(o, sizeof(o), "%x %X %o %c %%", 255, 255, 8, 'Z');
    CHECK(21, strcmp(o, "ff FF 10 Z %") == 0);
    snprintf(o, sizeof(o), "[%5d][%-5d][%05d][%5s][%-5s]", 42, 42, 42, "ab", "ab");
    CHECK(22, strcmp(o, "[   42][42   ][00042][   ab][ab   ]") == 0);
    snprintf(o, sizeof(o), "%lld %llu %zu", -5000000000LL, 18446744073709551615ULL, (usize)77);
    CHECK(23, strcmp(o, "-5000000000 18446744073709551615 77") == 0);
    snprintf(o, sizeof(o), "%p %08x %hhu %hd", (void *)0x1234, 0xBEEF, 300, 70000);
    CHECK(24, strcmp(o, "0x1234 0000beef 44 4464") == 0);
    snprintf(o, sizeof(o), "%s|%s|%-3c|", "x", (const char *)0, 'q');
    CHECK(25, strcmp(o, "x|(null)|q  |") == 0);
    snprintf(o, sizeof(o), "%05d %d", -42, 0);
    CHECK(26, strcmp(o, "-0042 0") == 0);

    /* a result that does not fit is cut, still NUL-terminated, and the full length is returned */
    char small[6];
    CHECK(27, snprintf(small, sizeof(small), "%s", "0123456789") == 10 && strcmp(small, "01234") == 0);
    CHECK(28, snprintf(small, 1, "abc") == 3 && small[0] == 0);
    CHECK(29, snprintf(0, 0, "abc%d", 12) == 5);                                /* just measuring */
    CHECK(30, snprintf(o, sizeof(o), "100%%%") == 4 && strcmp(o, "100%") == 0); /* a lone % at the end is dropped */

    /* printf itself: the return value is what was written (the text shows on the console) */
    CHECK(31, printf("libctest: printf %d %s\n", 7, "ok") == 22);

    if (fail == 0)
        puts("libctest: all checks passed");
    else
        printf("libctest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 44 : fail);
}
