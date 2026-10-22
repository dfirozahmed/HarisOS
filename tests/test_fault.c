/* Host test for fault_fmt.c: the CPU exception report text. */
#include <stdio.h>
#include <string.h>
#include "fault.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* which vectors carry an error code: must agree with the stubs in fault.c */
    static const int with_err[] = {8, 10, 11, 12, 13, 14, 17, 21, 29, 30};
    int n = 0;
    for (unsigned v = 0; v < 32; v++)
        n += fault_has_error_code(v);
    CHECK(n == 10);
    for (unsigned i = 0; i < sizeof(with_err) / sizeof(with_err[0]); i++)
        CHECK(fault_has_error_code((unsigned)with_err[i]));
    CHECK(!fault_has_error_code(6));
    CHECK(!fault_has_error_code(32)); /* not an exception */

    CHECK(strcmp(fault_name(14), "#PF PAGE FAULT") == 0);
    CHECK(strcmp(fault_name(6), "#UD INVALID OPCODE") == 0);
    CHECK(strcmp(fault_name(200), "INTERRUPT") == 0);
    for (unsigned v = 0; v < 32; v++)
        CHECK(fault_name(v) != 0 && fault_name(v)[0] != 0);

    /* page fault: write to a not-present kernel page */
    FaultFrame f;
    memset(&f, 0, sizeof(f));
    f.vector = 14;
    f.error = 2;
    f.rip = 0xFFFF800000123456ULL;
    f.cs = 8;
    f.rflags = 0x202;
    f.rsp = 0x7000;
    f.ss = 0x10;
    f.rax = 0x1122334455667788ULL;
    f.r15 = 0xAABBCCDDEEFF0011ULL;

    char buf[1024];
    int len = fault_format(buf, sizeof(buf), &f, 0xDEAD0000ULL);
    CHECK(len == (int)strlen(buf));
    CHECK(strstr(buf, "#PF PAGE FAULT  VEC=0E  ERR=0000000000000002") != 0);
    CHECK(strstr(buf, "NOT PRESENT, WRITE, KERNEL MODE") != 0);
    CHECK(strstr(buf, "RIP=FFFF800000123456  CS=0008  RFLAGS=0000000000000202") != 0);
    CHECK(strstr(buf, "RSP=0000000000007000  SS=0010") != 0);
    CHECK(strstr(buf, "CR2=00000000DEAD0000") != 0);
    CHECK(strstr(buf, "RAX=1122334455667788") != 0);
    CHECK(strstr(buf, "R15=AABBCCDDEEFF0011") != 0);

    /* user-mode instruction fetch on a protected page */
    f.error = 1 | 4 | 16;
    fault_format(buf, sizeof(buf), &f, 0);
    CHECK(strstr(buf, "PROTECTION VIOLATION, INSTRUCTION FETCH, USER MODE") != 0);

    /* a vector without error code prints no ERR field and no page fault line */
    f.vector = 6;
    f.error = 0;
    fault_format(buf, sizeof(buf), &f, 0);
    CHECK(strstr(buf, "#UD INVALID OPCODE  VEC=06\n") != 0);
    CHECK(strstr(buf, "ERR=") == 0);
    CHECK(strstr(buf, "KERNEL MODE") == 0);

    /* a small buffer is cut off, never overrun, and stays NUL-terminated */
    char small[16];
    memset(small, 'x', sizeof(small));
    len = fault_format(small, 10, &f, 0);
    CHECK(len == 9);
    CHECK(small[9] == 0);
    CHECK(small[10] == 'x');
    CHECK(fault_format(small, 0, &f, 0) == 0);

    if (fails == 0)
        printf("fault tests passed\n");
    return fails ? 1 : 0;
}
