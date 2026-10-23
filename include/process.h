/* ============================================================
 * HarisOS user processes: ring 3 programs
 *
 *   int id = process_start("NAME", image, size);   run the machine code in image[0..size) in ring 3
 *
 * A process is a kernel thread with its own address space (aspace.h). process_start builds the space (the image at
 * PAGING_USER_BASE, readable + executable but not writable; a 16 KiB stack at the top, readable + writable but not
 * executable), makes the thread, and returns at once; the thread enters ring 3 at the first byte of the image the first
 * time it is scheduled. The image is a flat binary (no headers) that must not depend on its load address. It
 * talks to the kernel with the "syscall" instruction (syscall.h).
 *
 * A process ends when it calls SYS_EXIT, or when it causes a CPU exception (a bad memory access, an illegal instruction,
 * a division by zero): the kernel reports it on the serial port, ends that process only, and carries on. Its thread,
 * stack, pages and page tables are freed by the scheduler.
 * ============================================================ */
#ifndef HARIS_PROCESS_H
#define HARIS_PROCESS_H

#include "fault.h"

#define PROCESS_IMAGE_MAX (64 * 1024)
#define PROCESS_STACK_PAGES 4

/* The thread id (see task.h) or -1: bad image, no memory, no free thread slot. Nothing stays allocated on failure. */
int process_start(const char *name, const void *image, unsigned long long size);

/* End the running process (called by SYS_EXIT, and by the kernel itself). Does not return. */
__attribute__((noreturn)) void process_exit(int code);

/* A CPU exception from ring 3 (f->cs has RPL 3): report it, end the process. Does not return. */
__attribute__((noreturn)) void process_fault(const FaultFrame *f, unsigned long long cr2);

typedef struct
{
    unsigned started, exited, faulted; /* processes started / ended by SYS_EXIT / ended by an exception */
    int last_exit;                     /* the exit code of the last one that ended by SYS_EXIT */
} ProcessStats;

void process_stats(ProcessStats *out);

/* Boot self test: runs four small programs in ring 3 (a write + exit, a bad pointer, a bad memory access, and one that
 * spins until the timer interrupt has taken the CPU from it three times) and checks what the kernel did, and that
 * nothing leaked. 0 = fine, otherwise a negative number that says which check failed. */
int process_selftest(void);

#endif
