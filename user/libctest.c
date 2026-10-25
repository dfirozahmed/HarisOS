/* HarisOS user program: checks the user C library (libc.c): string and memory functions, number parsing and printf.
 * Exits with 44 if every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

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

    /* strings, numbers, qsort */
    char cat[16] = "ab";
    strcat(cat, "cd");
    strncat(cat, "efgh", 2);
    CHECK(32, strcmp(cat, "abcdef") == 0 && ((char *)memchr("hello", 'l', 5))[1] == 'l' && memchr("hello", 'z', 5) == 0);
    char *dup = strdup("copy");
    CHECK(33, dup && dup != 0 && strcmp(dup, "copy") == 0);
    free(dup);
    CHECK(34, isupper('A') && !isupper('a') && islower('a') && isalnum('7') && isalnum('x') && !isalnum('-') && isprint(' ') && !isprint(10));
    CHECK(35, strtoull("18446744073709551615", 0, 10) == 18446744073709551615ULL && strtoull("-5", &end, 10) == 0 && abs(-3) == 3 && llabs(-5000000000LL) == 5000000000LL);
    int nums[] = {5, 3, 9, 1, 7, 3, 0, 8};
    qsort(nums, 8, sizeof(int), cmp_int);
    CHECK(36, nums[0] == 0 && nums[1] == 1 && nums[2] == 3 && nums[3] == 3 && nums[4] == 5 && nums[5] == 7 && nums[6] == 8 && nums[7] == 9);
    srand(1);
    int r1 = rand();
    srand(1);
    CHECK(37, rand() == r1 && r1 >= 0);

    /* stdio on a real file (RAM Disk) */
    FILE *f = fopen("/RAM Disk/libctest.tmp", "w");
    CHECK(38, f != 0);
    if (f)
    {
        CHECK(39, fprintf(f, "line %d\n", 1) == 7 && fputs("second\n", f) >= 0 && fputc('Z', f) == 'Z');
        CHECK(40, fwrite("0123456789", 1, 10, f) == 10);
        CHECK(41, fclose(f) == 0);
        f = fopen("/RAM Disk/libctest.tmp", "r");
        CHECK(42, f != 0);
        if (f)
        {
            char line[32];
            CHECK(43, fgets(line, sizeof(line), f) && strcmp(line, "line 1\n") == 0);
            CHECK(44, fgets(line, sizeof(line), f) && strcmp(line, "second\n") == 0 && ftell(f) == 14);
            int c = fgetc(f);
            CHECK(45, c == 'Z' && ungetc(c, f) == 'Z' && fgetc(f) == 'Z');
            CHECK(46, fseek(f, -4, SEEK_END) == 0 && fread(line, 1, 4, f) == 4 && memcmp(line, "6789", 4) == 0 && fgetc(f) == EOF && feof(f));
            rewind(f);
            CHECK(47, !feof(f) && fgetc(f) == 'l' && fseek(f, 1, SEEK_CUR) == 0 && fgetc(f) == 'n');
            fclose(f);
        }
        CHECK(48, remove("/RAM Disk/libctest.tmp") == 0 && fopen("/RAM Disk/libctest.tmp", "r") == 0);
    }
    CHECK(49, fopen("/RAM Disk/x", "r+") == 0 && fprintf(stdout, "libctest: stdout %s\n", "ok") == 20);

    /* pthread mutex / condition variable (single process: lock, trylock, unlock) */
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    CHECK(50, pthread_mutex_lock(&mu) == 0 && pthread_mutex_trylock(&mu) != 0 && pthread_mutex_unlock(&mu) == 0 && pthread_mutex_trylock(&mu) == 0);
    CHECK(51, pthread_mutex_unlock(&mu) == 0 && pthread_mutex_unlock(&mu) == EPERM && pthread_mutex_destroy(&mu) == 0);
    CHECK(52, pthread_cond_init(&cv, 0) == 0 && pthread_cond_signal(&cv) == 0 && pthread_cond_broadcast(&cv) == 0 && pthread_cond_destroy(&cv) == 0);

    if (fail == 0)
        puts("libctest: all checks passed");
    else
        printf("libctest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 44 : fail);
}
