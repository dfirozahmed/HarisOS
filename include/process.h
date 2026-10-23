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

/* The same for an ELF64 executable (elf.h): each PT_LOAD segment is mapped at its own address with its own permissions
 * (read, write, execute, never write + execute), the entry point is the file's. `file` is only read, and may be freed
 * as soon as this returns. Returns the thread id, or -1 with *err (if not NULL) = the ELF_ERR_* reason (a file that is
 * not acceptable) or 0 (out of memory or thread slots). Nothing stays allocated on failure. */
int process_start_elf(const char *name, const void *file, unsigned long long size, int console, int *err);

/* What the kernel keeps for a running process besides its address space (the thread's `data`, see task.h). The system
 * calls reach it with process_current(). */
#define PROCESS_MAX_FILES 5
#define PROCESS_FD_FIRST 3 /* file descriptors 0, 1 and 2 are the console: input, output, errors */
#define PROCESS_HEAP_MAX (64ULL * 1024 * 1024) /* the most SYS_BRK lets a program grow its heap to */
typedef struct Process
{
    int console; /* console.h handle of its window, -1 = none (output goes to the serial port, input is empty) */
    /* The heap (SYS_BRK): heap_base is page aligned, just above the program's last segment. heap_brk is the program's
     * break (heap_base ... heap_limit), heap_mapped the end of the pages really mapped (page aligned, >= heap_brk
     * rounded up). Lowering the break keeps the pages: they are given back when the process ends. */
    unsigned long long heap_base, heap_brk, heap_mapped, heap_limit;
    struct
    {
        int node;     /* VFS node of an open file, -1 = this slot is free (file descriptor PROCESS_FD_FIRST + index) */
        unsigned pos; /* the read position */
    } file[PROCESS_MAX_FILES];
} Process;

Process *process_current(void); /* the running process, NULL in a kernel thread */

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

/* How the process that ran as thread `id` ended: its exit code, or -1 if a CPU exception ended it. Valid after the thread has
 * ended and until its slot is used by another thread (read it right after process_wait). 0 if it never ran / still runs. */
int process_exit_code(int id);

/* Give the CPU to other threads until process thread `id` has ended and been cleaned up (its stack, pages and page
 * tables freed). 0 = done, -1 = still running after a few hundred turns (a program that keeps running). For tests and for
 * code that must wait for a program; a normal caller just lets the scheduler run. */
int process_wait(int id);

/* Boot self test: runs four small programs in ring 3 (a write + exit, a bad pointer, a bad memory access, and one that
 * spins until the timer interrupt has taken the CPU from it three times) and checks what the kernel did, and that
 * nothing leaked. 0 = fine, otherwise a negative number that says which check failed. */
int process_selftest(void);

#endif
