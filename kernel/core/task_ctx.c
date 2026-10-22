/* HarisOS context switch: see include/task.h. */
#include "task.h"

#if defined(__x86_64__)

/* Microsoft x64 ABI (the kernel is built for x86_64-pc-win32-coff): a function may change rax rcx rdx r8-r11 and
 * xmm0-5, but must keep rbx rbp rdi rsi r12-r15 and xmm6-15. task_ctx_switch looks like a normal call to its caller,
 * so only those have to be saved. Arguments: rcx = save_rsp, rdx = new_rsp.
 *
 * The save order here is mirrored by TaskInitFrame in task.c, which builds the stack of a new thread. xmm uses
 * movups, not movaps: the stack of a new thread is not aligned the way a switched-out one is. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl task_ctx_switch\n"
        "task_ctx_switch:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $160, %rsp\n"
        "    movups %xmm6, 0(%rsp)\n"
        "    movups %xmm7, 16(%rsp)\n"
        "    movups %xmm8, 32(%rsp)\n"
        "    movups %xmm9, 48(%rsp)\n"
        "    movups %xmm10, 64(%rsp)\n"
        "    movups %xmm11, 80(%rsp)\n"
        "    movups %xmm12, 96(%rsp)\n"
        "    movups %xmm13, 112(%rsp)\n"
        "    movups %xmm14, 128(%rsp)\n"
        "    movups %xmm15, 144(%rsp)\n"
        "    movq %rsp, (%rcx)\n" /* the old thread is frozen here */
        "    movq %rdx, %rsp\n"   /* ... and the new one continues */
        "    movups 0(%rsp), %xmm6\n"
        "    movups 16(%rsp), %xmm7\n"
        "    movups 32(%rsp), %xmm8\n"
        "    movups 48(%rsp), %xmm9\n"
        "    movups 64(%rsp), %xmm10\n"
        "    movups 80(%rsp), %xmm11\n"
        "    movups 96(%rsp), %xmm12\n"
        "    movups 112(%rsp), %xmm13\n"
        "    movups 128(%rsp), %xmm14\n"
        "    movups 144(%rsp), %xmm15\n"
        "    addq $160, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n");

#endif
