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
#include "pipe.h"

#define PROCESS_IMAGE_MAX (64 * 1024)
#define PROCESS_STACK_PAGES 4

/* The thread id (see task.h) or -1: bad image, no memory, no free thread slot. Nothing stays allocated on failure. */
int process_start(const char *name, const void *image, unsigned long long size);

/* The same for an ELF64 executable (elf.h): each PT_LOAD segment is mapped at its own address with its own permissions
 * (read, write, execute, never write + execute), the entry point is the file's. `file` is only read, and may be freed
 * as soon as this returns. Returns the thread id, or -1 with *err (if not NULL) = the ELF_ERR_* reason (a file that is
 * not acceptable) or 0 (out of memory or thread slots). Nothing stays allocated on failure. */
int process_start_elf(const char *name, const void *file, unsigned long long size, int console, int *err);

/* How a process is started when it is more than a file: with arguments, and as the child of another process.
 *   console   handle of the console window it writes to / reads from (console.h), -1 = none
 *   argc,args the arguments: `args` holds argc NUL-terminated strings one after the other, args_len bytes in all (at most
 *             PROCESS_ARGS_MAX strings and PROCESS_ARGS_BYTES bytes). They are copied to the top of the new stack and the
 *             program starts as _start(argc, argv): rcx / rdi = argc, rdx / rsi = argv (argv[argc] = NULL).
 *   parent    thread id of the process that spawned it, -1 = started by the kernel. A child shares its parent's console
 *             (and does not write the "[process exited]" line: the owner of the window does), and when it ends its
 *             exit code is kept for the parent's SYS_WAIT (process_reap) and the parent is woken.
 *   pid       out: the new process's number (1, 2, 3 ... never reused), set when the start succeeds. */
#define PROCESS_ARGS_MAX 32
#define PROCESS_ARGS_BYTES 2048
#define PROCESS_MAX_CHILDREN 8

/* An open file of a process. */
#define PROCESS_MAX_FILES 8
#define PROCESS_FD_FIRST 3 /* file descriptors 0, 1 and 2 are the console: input, output, errors (unless redirected) */
#define PFILE_WRITE 1      /* opened for writing (SYS_O_WRITE): reads are refused */
#define PFILE_APPEND 2     /* every write goes to the end */
typedef struct
{
    int node;       /* VFS node, -1 = this slot is free */
    unsigned pos;   /* the read / write position */
    unsigned flags; /* PFILE_* */
} ProcessFile;

/* A slot can also hold one end of a pipe (pipe.h): node = PROCESS_PIPE_NODE + the pipe's number, flags has PFILE_WRITE for the
 * write end. Such a slot is "open" like any other (node >= 0) but is not a VFS node. */
#define PROCESS_PIPE_NODE 0x40000000
static inline int pfile_is_pipe(const ProcessFile *f) { return f->node >= PROCESS_PIPE_NODE; }
static inline int pfile_pipe(const ProcessFile *f) { return f->node - PROCESS_PIPE_NODE; }

/* Close an open slot: a pipe end is given back to the pipe (waking whoever waits on it). Safe on a free slot. */
static inline void pfile_close(ProcessFile *f)
{
    if (f->node >= 0 && pfile_is_pipe(f))
        pipe_unref(pfile_pipe(f), (f->flags & PFILE_WRITE) != 0);
    f->node = -1;
    f->pos = f->flags = 0;
}

typedef struct
{
    int console;
    int argc;
    const char *args;
    unsigned args_len;
    int parent;
    int cwd; /* working folder (a VFS node id, 0 = the root); a child normally gets its parent's */
    int pid;
    /* NULL, or three files that stand in for fd 0, 1, 2 of the child (node < 0 = leave that one on the console). Two entries
     * with the same node and flags share one open file (and so one position): "cmd > out 2>&1". */
    const ProcessFile *redirect;
} ProcessSpawn;

/* process_start_elf with a ProcessSpawn: returns the thread id or -1 (*err as there). */
int process_spawn_elf(const char *name, const void *file, unsigned long long size, ProcessSpawn *sp, int *err);

/* Has the child `pid` ended? If so: 1, its exit code in *code (-1 = ended by a CPU exception), and the record is used up
 * (a second call says 0). 0 = still running or unknown. The records of the last few dozen children are kept. */
int process_reap(int pid, int *code);

/* What the kernel keeps for a running process besides its address space (the thread's `data`, see task.h). The system
 * calls reach it with process_current(). */
#define PROCESS_HEAP_MAX (64ULL * 1024 * 1024) /* the most SYS_BRK lets a program grow its heap to */
typedef struct Process
{
    int console; /* console.h handle of its window, -1 = none (output goes to the serial port, input is empty) */
    int own_console;                  /* 1 = started by the kernel / File Explorer: it writes the "[process exited]" line */
    int pid;                          /* its number, see ProcessSpawn */
    int parent;                       /* thread id of the parent, -1 = none */
    int cwd;                          /* VFS node of the working folder (VFS_ROOT = "This PC"): relative paths start there */
    int child[PROCESS_MAX_CHILDREN];  /* pids of children not yet waited for, 0 = free slot */
    /* The heap (SYS_BRK): heap_base is page aligned, just above the program's last segment. heap_brk is the program's
     * break (heap_base ... heap_limit), heap_mapped the end of the pages really mapped (page aligned, >= heap_brk
     * rounded up). Lowering the break keeps the pages: they are given back when the process ends. */
    unsigned long long heap_base, heap_brk, heap_mapped, heap_limit;
    ProcessFile file[PROCESS_MAX_FILES]; /* file descriptor PROCESS_FD_FIRST + index */
    int stdio[3]; /* fd 0, 1, 2: -1 = the console, else the index in file[] that stands in for it (shell redirection) */
} Process;

Process *process_current(void); /* the running process, NULL in a kernel thread */

/* Close every open file of the process (pipes: the ends go back to the pipe). Called when it ends; harmless to call twice. */
void process_release_files(Process *p);

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
