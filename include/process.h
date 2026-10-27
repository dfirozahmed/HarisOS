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
#include "heap.h"
#include "mq.h"
#include "pipe.h"
#include "sem.h"
#include "usync.h"
#include "shm.h"
#include "sock.h"

#define PROCESS_IMAGE_MAX (64 * 1024)
#define PROCESS_STACK_PAGES 4

/* The thread id (see task.h) or -1: bad image, no memory, no free thread slot. Nothing stays allocated on failure. */
int process_start(const char *name, const void *image, unsigned long long size);

/* A built-in program that only keeps a processor busy (the Task Manager's per-processor button), pinned to application processor `cpu`
 * (task_set_affinity). process_spin_start: the thread id, or -1 (no such processor, no clock, no memory). It ends by itself after ten
 * minutes. process_spin_stop(id): tell it to end; harmless for an id that is gone or that is not a SPIN program. */
int process_spin_start(int cpu);
void process_spin_stop(int id);

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
#define PROCESS_ARGS_MAX 4096           /* the most arguments (strings) one program can be started with */
#define PROCESS_ARGS_BYTES (256 * 1024) /* ... and the most bytes in them together: they are copied to the top of the new stack, which is made big enough */
#define PROCESS_MAX_CHILDREN 1024 /* children one process can have outstanding: a sanity cap, the list grows as needed (the thread slots run out first) */

/* An open file of a process. */
#define PROCESS_MAX_FILES 1024 /* the most open files (and pipe ends, queues, ...) one process can have: a sanity cap, the list grows as needed */
#define PROCESS_FILES_FIRST 8  /* entries a new process starts with */
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
/* ... or a message queue (mq.h): node = PROCESS_MQ_NODE + the queue's number; it is read and written like a pipe end that does both. */
#define PROCESS_MQ_NODE 0x50000000
static inline int pfile_is_pipe(const ProcessFile *f) { return f->node >= PROCESS_PIPE_NODE && f->node < PROCESS_MQ_NODE; }
/* ... or a semaphore (sem.h): node = PROCESS_SEM_NODE + its number. SYS_SEM_* work on it; it cannot be read, written or seeked. */
#define PROCESS_SEM_NODE 0x60000000
static inline int pfile_is_mq(const ProcessFile *f) { return f->node >= PROCESS_MQ_NODE && f->node < PROCESS_SEM_NODE; }
/* ... or a named mutex (usync.h): node = PROCESS_MUTEX_NODE + its number; or a condition variable: node = PROCESS_COND_NODE + its number.
 * SYS_MUTEX_* / SYS_COND_* work on them; they cannot be read, written or seeked. */
#define PROCESS_MUTEX_NODE 0x70000000
#define PROCESS_COND_NODE 0x78000000
/* ... or a socket (sock.h): node = PROCESS_SOCK_NODE + its number. SYS_READ / SYS_WRITE receive and send on it; it cannot be seeked. */
#define PROCESS_SOCK_NODE 0x7C000000
static inline int pfile_is_sem(const ProcessFile *f) { return f->node >= PROCESS_SEM_NODE && f->node < PROCESS_MUTEX_NODE; }
static inline int pfile_is_mutex(const ProcessFile *f) { return f->node >= PROCESS_MUTEX_NODE && f->node < PROCESS_COND_NODE; }
static inline int pfile_is_cond(const ProcessFile *f) { return f->node >= PROCESS_COND_NODE && f->node < PROCESS_SOCK_NODE; }
static inline int pfile_is_sock(const ProcessFile *f) { return f->node >= PROCESS_SOCK_NODE; }
static inline int pfile_sock(const ProcessFile *f) { return f->node - PROCESS_SOCK_NODE; }
static inline int pfile_is_usync(const ProcessFile *f) { return f->node >= PROCESS_MUTEX_NODE && f->node < PROCESS_SOCK_NODE; }
static inline int pfile_usync(const ProcessFile *f) { return f->node >= PROCESS_COND_NODE ? f->node - PROCESS_COND_NODE : f->node - PROCESS_MUTEX_NODE; }
static inline int pfile_sem(const ProcessFile *f) { return f->node - PROCESS_SEM_NODE; }
static inline int pfile_is_object(const ProcessFile *f) { return f->node >= PROCESS_MQ_NODE; } /* a queue or a semaphore: not a stdin / stdout */
static inline int pfile_mq(const ProcessFile *f) { return f->node - PROCESS_MQ_NODE; }
static inline int pfile_pipe(const ProcessFile *f) { return f->node - PROCESS_PIPE_NODE; }

/* One more holder of the pipe end or queue this slot names (a copy of the slot was made: fork, spawn, dup2). Nothing for a file. */
static inline void pfile_ref(const ProcessFile *f)
{
    if (f->node >= 0 && pfile_is_pipe(f))
        pipe_ref(pfile_pipe(f), (f->flags & PFILE_WRITE) != 0);
    else if (f->node >= 0 && pfile_is_mq(f))
        mq_ref(pfile_mq(f));
    else if (f->node >= 0 && pfile_is_sem(f))
        sem_ref(pfile_sem(f));
    else if (f->node >= 0 && pfile_is_usync(f))
        usync_ref(pfile_usync(f));
    else if (f->node >= 0 && pfile_is_sock(f))
        sock_ref(pfile_sock(f));
}

/* Close an open slot: a pipe end is given back to the pipe (waking whoever waits on it). Safe on a free slot. */
static inline void pfile_close(ProcessFile *f)
{
    if (f->node >= 0 && pfile_is_pipe(f))
        pipe_unref(pfile_pipe(f), (f->flags & PFILE_WRITE) != 0);
    else if (f->node >= 0 && pfile_is_mq(f))
        mq_unref(pfile_mq(f));
    else if (f->node >= 0 && pfile_is_sem(f))
        sem_unref(pfile_sem(f));
    else if (f->node >= 0 && pfile_is_usync(f))
        usync_unref(pfile_usync(f));
    else if (f->node >= 0 && pfile_is_sock(f))
        sock_unref(pfile_sock(f));
    f->node = -1;
    f->pos = f->flags = 0;
}

typedef struct
{
    int console;
    int argc;
    const char *args;
    unsigned args_len;
    int free_args; /* process_exec_elf only: 1 = `args` is a kernel heap block that process_exec_elf frees itself just before the point of no return
                    * (after that nothing returns to the caller that could free it); on failure it is left alone */
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
/* A mapping of a shared memory segment (shm.h) in this process: where, how many pages. */
#define PROCESS_MAX_SHM 1024 /* the most mappings one process can have: a sanity cap, the list below grows as needed */
#define PROCESS_SHM_BASE 0x0000500000000000ULL /* the area the kernel picks addresses from, far from the program and its stack */
#define PROCESS_SHM_END 0x0000600000000000ULL
typedef struct
{
    int seg; /* shm.h segment, -1 = this slot is free */
    unsigned long long addr;
    unsigned pages;
} ProcessShm;

typedef struct Process
{
    int console; /* console.h handle of its window, -1 = none (output goes to the serial port, input is empty) */
    int own_console;                  /* 1 = started by the kernel / File Explorer: it writes the "[process exited]" line */
    int pid;                          /* its number, see ProcessSpawn */
    int parent;                       /* thread id of the parent, -1 = none */
    int cwd;                          /* VFS node of the working folder (VFS_ROOT = "This PC"): relative paths start there */
    unsigned sig_ignore;              /* bit n set: signal n is ignored (SYS_SIGIGNORE); 0 at the start of every process */
    unsigned long long sig_handler[32]; /* handlers (SYS_SIGACTION), 0 = none: the default action; only SIGINT and SIGTERM can have one */
    unsigned long long sig_entry;     /* the user code stub that calls a handler and then SYS_SIGRETURN (usersig.h) */
    unsigned sig_pending, sig_blocked; /* bit n: signal n waits for its handler / must wait (it is being handled) */
    ProcessShm *shm;                  /* shared memory mapped here (SYS_SHM_MAP): a heap block of shm_cap entries (NULL when none), grown when full */
    int shm_cap;
    unsigned long long shm_next;      /* where the next mapping goes (0 = PROCESS_SHM_BASE) */
    int *child;                       /* pids of children not yet waited for, 0 = free slot: a heap block of child_cap entries (NULL when none), grown when full */
    int child_cap;
    /* The heap (SYS_BRK): heap_base is page aligned, just above the program's last segment. heap_brk is the program's
     * break (heap_base ... heap_limit), heap_mapped the end of the pages really mapped (page aligned, >= heap_brk
     * rounded up). Lowering the break keeps the pages: they are given back when the process ends. */
    unsigned long long heap_base, heap_brk, heap_mapped, heap_limit;
    /* The stack (user half, from PAGING_USER_END down) starts with PROCESS_STACK_PAGES pages (more if the arguments need them) and grows
     * downwards by itself, a page fault at a time, down to ELF_STACK_RESERVE bytes below the top. stack_low = the lowest page mapped. */
    unsigned long long stack_low;
    ProcessFile *file; /* file descriptor PROCESS_FD_FIRST + index: a heap block of file_cap entries, made with the process and grown when full */
    int file_cap;
    int stdio[3]; /* fd 0, 1, 2: -1 = the console, else the index in file[] that stands in for it (shell redirection) */
} Process;

Process *process_current(void); /* the running process, NULL in a kernel thread */

/* The list of open files. process_files_init: file_cap = PROCESS_FILES_FIRST entries, all free (node -1); 0, or -1 when the heap is out of memory.
 * process_file_slot: the index of a free entry, the list doubled first if every entry is in use (up to PROCESS_MAX_FILES); -1 at the limit or when
 * the heap is out of memory (nothing changed). A slot is named by its index, never by address: growing may move the list. process_files_copy:
 * dst gets a list of its own with the same entries (the caller counts the new holders with pfile_ref); 0, or -1 for no memory. */
static inline void pfile_clear(ProcessFile *f)
{
    f->node = -1;
    f->pos = f->flags = 0;
}

static inline int process_files_init(Process *p)
{
    p->file = kmalloc((unsigned long long)PROCESS_FILES_FIRST * sizeof(ProcessFile));
    p->file_cap = 0;
    if (!p->file)
        return -1;
    p->file_cap = PROCESS_FILES_FIRST;
    for (int i = 0; i < p->file_cap; i++)
        pfile_clear(&p->file[i]);
    return 0;
}

static inline int process_file_slot(Process *p)
{
    for (int i = 0; i < p->file_cap; i++)
        if (p->file[i].node < 0)
            return i;
    if (p->file_cap >= PROCESS_MAX_FILES)
        return -1;
    int cap = p->file_cap ? p->file_cap * 2 : PROCESS_FILES_FIRST;
    if (cap > PROCESS_MAX_FILES)
        cap = PROCESS_MAX_FILES;
    ProcessFile *n = krealloc(p->file, (unsigned long long)cap * sizeof(ProcessFile));
    if (!n)
        return -1;
    int first = p->file_cap;
    for (int i = first; i < cap; i++)
        pfile_clear(&n[i]);
    p->file = n;
    p->file_cap = cap;
    return first;
}

static inline int process_files_copy(Process *dst, const Process *src)
{
    dst->file = kmalloc((unsigned long long)src->file_cap * sizeof(ProcessFile));
    dst->file_cap = 0;
    if (!dst->file)
        return -1;
    for (int i = 0; i < src->file_cap; i++)
        dst->file[i] = src->file[i];
    dst->file_cap = src->file_cap;
    return 0;
}

/* Close every open file of the process (pipes: the ends go back to the pipe). Called when it ends; harmless to call twice. */
void process_release_files(Process *p);

/* A free entry of p->shm for a new mapping: the list is doubled (8, 16, ...) when every entry is in use, up to PROCESS_MAX_SHM. The
 * pointer is good until the next call (the list may move). NULL at the limit or when the heap is out of memory (nothing changed). */
static inline ProcessShm *process_shm_slot(Process *p)
{
    for (int i = 0; i < p->shm_cap; i++)
        if (p->shm[i].seg < 0)
            return &p->shm[i];
    if (p->shm_cap >= PROCESS_MAX_SHM)
        return 0;
    int cap = p->shm_cap ? p->shm_cap * 2 : 8;
    if (cap > PROCESS_MAX_SHM)
        cap = PROCESS_MAX_SHM;
    ProcessShm *n = krealloc(p->shm, (unsigned long long)cap * sizeof(ProcessShm));
    if (!n)
        return 0;
    for (int i = p->shm_cap; i < cap; i++)
        n[i].seg = -1;
    p->shm = n;
    int first = p->shm_cap;
    p->shm_cap = cap;
    return &n[first];
}

/* A free entry of p->child for a new child (the list is doubled, 8, 16, ..., when all are in use). The index, or -1 at the limit or without
 * memory. Entries are named by index: growing may move the list. */
static inline int process_child_slot(Process *p)
{
    for (int i = 0; i < p->child_cap; i++)
        if (p->child[i] == 0)
            return i;
    if (p->child_cap >= PROCESS_MAX_CHILDREN)
        return -1;
    int cap = p->child_cap ? p->child_cap * 2 : 8;
    if (cap > PROCESS_MAX_CHILDREN)
        cap = PROCESS_MAX_CHILDREN;
    int *n = krealloc(p->child, (unsigned long long)cap * sizeof(int));
    if (!n)
        return -1;
    int first = p->child_cap;
    for (int i = first; i < cap; i++)
        n[i] = 0;
    p->child = n;
    p->child_cap = cap;
    return first;
}

/* Let go of the shared memory segments this process maps (their reference counts; the pages go away with its address space).
 * process_release_files does this too; exec calls it alone, because its files stay. */
static inline void process_release_shm(Process *p)
{
    if (!p)
        return;
    for (int i = 0; i < p->shm_cap; i++)
        if (p->shm[i].seg >= 0)
            shm_unref(p->shm[i].seg);
    kfree(p->shm); /* the list itself (NULL is fine) */
    p->shm = 0;
    p->shm_cap = 0;
    p->shm_next = 0;
}

/* Signals (syscall.h): end process `pid` with exit code 128 + sig, unless it ignores sig (SIGKILL cannot be ignored); sig 0 only
 * looks. 0, or -1 if there is no such process. A process that signals itself ends at once (does not return). */
int process_signal(int pid, int sig);

/* The scheduling priority (task.h) of process `pid`: returns the one it had (0 .. TASK_PRIO_MAX), and sets prio unless it is -1 (only
 * look). -1 if there is no such process or prio is out of range. */
int process_priority(int pid, int prio);

/* Ctrl+C in console `con`: SIGINT to every process that writes to it. (console_set_interrupt_hook installs this.) */
void process_interrupt_console(int con);

/* The index-th live process, for ps: 1 and *out filled, 0 when there is none. */
typedef struct
{
    int pid, ppid, running, console;
    char name[16];
} ProcessInfo;
int process_info(int index, ProcessInfo *out);

/* exec: replace the running process's program by the ELF file (see process_spawn_elf for the arguments; `args` supplies argc / args only).
 * free_file != 0: the kernel heap block `file` is freed just before the point of no return (the caller can not do it).
 * Does not return when it works. -1 with *err = the ELF_ERR_* reason or 0 (no memory) if it fails, and then nothing has changed. */
int process_exec_elf(const char *name, const void *file, unsigned long long size, const ProcessSpawn *args, int *err, int free_file);

/* fork: a copy of the running process, resuming at user_rip / user_rsp with rax = 0 (the system call layer passes what SYSCALL saved).
 * Returns the child's pid, or -1. */
int process_fork(unsigned long long user_rip, unsigned long long user_rsp);

int process_ppid(void); /* the parent's pid, 0 if none */

/* A write to a read-only page that ring 3 attempted (page fault, error code 7): if it is a copy-on-write page of a forked process it is
 * made private and writable. 1 = the faulting instruction can run again, anything else = a real fault. */
int process_cow_fault(unsigned long long addr);

/* The user stack grows on demand. process_stack_fault: a page fault of ring 3 code at `addr` (page not present) with the stack pointer `rsp`: if
 * it is in the area the stack may grow into and not much further below rsp than a big stack frame, the pages down to it are mapped (zeroed)
 * and 1 is returned - the instruction runs again. 0 = a real fault. process_stack_grow: the same without the rsp rule, for the kernel when it
 * is about to use a user stack address (a system call at a stack pointer below the mapped part, a signal frame): 1 if `addr` is mapped
 * afterwards (it was, or it was made), 0 if it is not in the stack area or there is no memory. */
int process_stack_fault(unsigned long long addr, unsigned long long rsp);
int process_stack_grow(unsigned long long addr);

/* End the running process (called by SYS_EXIT, and by the kernel itself). Does not return. */
__attribute__((noreturn)) void process_exit(int code);

/* A CPU exception from ring 3 (f->cs has RPL 3): report it, end the process. Does not return. */
__attribute__((noreturn)) void process_fault(const FaultFrame *f, unsigned long long cr2);

typedef struct
{
    unsigned started, exited, faulted; /* processes started / ended by SYS_EXIT / ended by an exception */
    int last_exit;                     /* the exit code of the last one that ended by SYS_EXIT */
    unsigned killed;                   /* processes ended by a signal (their code is 128 + the signal) */
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
