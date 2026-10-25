/* <setjmp.h> for HarisOS user programs (implementation: user/posix.c). The buffer holds what a Microsoft x64 function must keep:
 * rbx rbp rdi rsi rsp r12-r15, the return address, and xmm6-xmm15 (this target's calling convention makes those callee-saved). */
#ifndef HARIS_SETJMP_H
#define HARIS_SETJMP_H

typedef struct
{
    unsigned long long r[32]; /* 10 integer slots (80 bytes) + 10 xmm registers (160 bytes) + spare */
} jmp_buf[1];

int setjmp(jmp_buf env) __attribute__((returns_twice));
int _setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
