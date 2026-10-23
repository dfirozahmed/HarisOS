/* HarisOS demo programs for ring 3, used by the boot self test (process_selftest). They are position independent
 * machine code, assembled into the kernel image and copied into a process's address space by process_start. */
#include "syscall.h"

#define STR_(x) #x
#define STR(x) STR_(x)

#if defined(__x86_64__)

/* Each program is the bytes between its _start and _end label. User code follows the syscall convention of syscall.h
 * (number in eax, arguments in rdi, rsi, rdx). */
__asm__(".text\n"
        ".p2align 4\n"

        /* write a line, then exit with code 7 */
        ".globl user_hello_start\n"
        ".globl user_hello_end\n"
        "user_hello_start:\n"
        "    movl $" STR(SYS_WRITE) ", %eax\n"
        "    leaq .Lhello_msg(%rip), %rdi\n"
        "    movl $(.Lhello_msg_end - .Lhello_msg), %esi\n"
        "    syscall\n"
        "    movl $" STR(SYS_EXIT) ", %eax\n"
        "    movl $7, %edi\n"
        "    syscall\n"
        "0:  jmp 0b\n"
        ".Lhello_msg:\n"
        "    .ascii \"hello from ring 3\\n\"\n"
        ".Lhello_msg_end:\n"
        "user_hello_end:\n"

        /* write from a kernel address: the kernel must refuse (-1); the program exits with that result as its code */
        ".p2align 4\n"
        ".globl user_badptr_start\n"
        ".globl user_badptr_end\n"
        "user_badptr_start:\n"
        "    movl $" STR(SYS_WRITE) ", %eax\n"
        "    movq $0x1000, %rdi\n"
        "    movl $16, %esi\n"
        "    syscall\n"
        "    movl %eax, %edi\n"
        "    movl $" STR(SYS_EXIT) ", %eax\n"
        "    syscall\n"
        "0:  jmp 0b\n"
        "user_badptr_end:\n"

        /* spin in ring 3 without ever calling the kernel, until the kernel writes a stop word into the program's own
         * page (it can: it reaches the page by its physical address) or the TSC reaches a deadline the kernel wrote
         * next to it. Exit code 5 = told to stop, 6 = deadline. Only the timer interrupt can take the CPU from it. */
        ".p2align 4\n"
        ".globl user_spin_start\n"
        ".globl user_spin_end\n"
        ".globl user_spin_stop\n"
        ".globl user_spin_deadline\n"
        "user_spin_start:\n"
        "1:  cmpl $0, user_spin_stop(%rip)\n"
        "    jne 2f\n"
        "    rdtsc\n"
        "    shlq $32, %rdx\n"
        "    orq %rdx, %rax\n"
        "    cmpq user_spin_deadline(%rip), %rax\n"
        "    jb 1b\n"
        "    movl $" STR(SYS_EXIT) ", %eax\n"
        "    movl $6, %edi\n"
        "    syscall\n"
        "2:  movl $" STR(SYS_EXIT) ", %eax\n"
        "    movl $5, %edi\n"
        "    syscall\n"
        "0:  jmp 0b\n"
        ".p2align 3\n"
        "user_spin_stop:\n"
        "    .quad 0\n"
        "user_spin_deadline:\n"
        "    .quad 0\n"
        "user_spin_end:\n"

        /* read kernel memory directly: a page fault from ring 3, which ends this process and nothing else */
        ".p2align 4\n"
        ".globl user_fault_start\n"
        ".globl user_fault_end\n"
        "user_fault_start:\n"
        "    movq $0x1000, %rax\n"
        "    movq (%rax), %rbx\n"
        "0:  jmp 0b\n"
        "user_fault_end:\n");

#endif
