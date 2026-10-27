/* Shared by the files of the kernel thread scheduler (kernel/sched/task/): the Task record, the table, the per processor state and the scheduler lock. The public interface is include/task.h. */
#ifndef HARIS_TASK_INTERNAL_H
#define HARIS_TASK_INTERNAL_H

#include "task.h"
#include "aspace.h"
#include "bkl.h"
#include "idle.h"
#include "fpu.h"
#include "heap.h"
#include "irq.h"
#include "paging.h"
#include "pmm.h"
#include "preempt.h"
#include "syscall.h"
#include "percpu.h"
#include "smp.h"
#include "timer.h"

/* A stack is a run of whole pages from the page allocator: one guard page at the bottom, then TASK_STACK_SIZE bytes.
 * The guard page is made not present (paging.h), so a stack that grows past its end stops with a page fault. */
#define STACK_USABLE_PAGES (TASK_STACK_SIZE / PMM_PAGE_SIZE)
#define STACK_BLOCK_PAGES (STACK_USABLE_PAGES + 1)

typedef struct
{
    unsigned long long rsp; /* saved stack pointer while the thread is not running */
    unsigned char *stack;   /* start of the stack block (the guard page), NULL for task 0 (the firmware's stack) */
    int guarded;            /* the guard page really is unmapped (needs the page tables to be reachable) */
    AddressSpace *as;       /* the user address space of a process thread, NULL for kernel threads (kernel tables) */
    void *data;             /* the process's own data (process.h), handed back by task_user_data() */
    TaskFreeFn free_data;   /* called with data when the thread is cleaned up (may be NULL) */
    unsigned long long kstack_top; /* top of the stack above: where the CPU lands when ring 3 code traps (TSS rsp0) */
    TaskState state;
    unsigned flags;         /* TASK_PREEMPTIBLE */
    int prio;               /* TASK_PRIO_MIN..TASK_PRIO_MAX */
    int inherit;            /* priority borrowed from a waiter on a lock this thread holds (task_set_inherited), -1 = none */
    unsigned waited;        /* schedule() passes in which this thread was READY but another one was chosen (aging) */
    unsigned long long wake_us; /* TASK_SLEEPING: when to become ready */
    void *held;             /* mutex.c: the list of mutexes this thread holds */
    void *blocked;          /* mutex.c: the mutex this thread sleeps on */
    int exit_code;          /* process.c: how the process of this thread ended (task_set_exit_code) */
    TaskFn fn;
    void *arg;
    char name[TASK_NAME_MAX];
    unsigned long long run_ticks; /* timer ticks that found this thread running (task_run_ticks) */
    int last_cpu;           /* the processor it was last switched in on (task_last_cpu), -1 = never */
    unsigned cpus_ran;      /* bit n: this thread has been switched in on processor n (task_cpu_mask) */
    int idle_cpu;           /* the idle context of application processor n-1 (an entry of task_register_idle), 0 = an ordinary thread */
    int kill_pending;       /* task_kill found it running on another processor: it ends at that processor's next timer tick */
    int affinity;           /* -1 = any processor that may run it (eligible), n = only processor n (task_set_affinity) */
    /* The floating point / SSE registers of a process thread while it is not running (see fpu.h). Only process threads use
     * it; kernel threads leave it alone. FXSAVE needs 16-byte alignment, which the attribute gives every Task. */
    unsigned char fpu[FPU_STATE_SIZE] __attribute__((aligned(16)));
} Task;

/* Written at the lowest usable address, just above the guard page. The guard page catches a stack that grows into it
 * one step at a time; a function with a huge local array can jump over it, and then this value is what gets damaged:
 * schedule() checks it whenever a thread leaves the CPU. */
#define STACK_CANARY 0x48617269734F5321ULL /* "HarisOS!" */
#define CANARY_AT(stack) (*(unsigned long long *)((stack) + PMM_PAGE_SIZE))

/* Interrupt rules. The table, CUR and SLICE_LEFT are changed by the scheduler, which the timer interrupt can also start
 * (task_tick). So every path into schedule() runs with interrupts off (irq_save), and so does everything that edits
 * the table (create, reap). A thread that is switched out keeps that state on its own stack and gets it back when
 * it is switched in again: it continues inside schedule(), and its caller then restores the flags it saved. */
/* The threads live in chunks of TASK_CHUNK, made with kmalloc when every slot so far is in use (create). A chunk is never freed: a Task
 * never moves, so the scheduler (which runs in the timer interrupt and must not allocate) and everybody who looks at a thread by its id
 * can read the table without a lock while a create adds a chunk. CAP is the number of slots in the chunks so far; it only grows, and
 * DIR[] is filled before CAP is raised. Slot 0 is the main thread. */
#define TASK_CHUNK 16
typedef struct
{
    Task t[TASK_CHUNK];
} TaskChunk;

extern TaskChunk *DIR[TASK_LIMIT / TASK_CHUNK];
extern int CAP;

static inline Task *tk(int id) { return &DIR[id / TASK_CHUNK]->t[id % TASK_CHUNK]; } /* id < CAP */

extern int CURS[SMP_MAX_CPUS];
extern unsigned SLICES[SMP_MAX_CPUS];
extern AddressSpace *ACTIVE[SMP_MAX_CPUS];
extern int FPU_HOLDS[SMP_MAX_CPUS];

#define CUR (CURS[percpu_index()])
#define SLICE_LEFT (SLICES[percpu_index()])
#define ACTIVE_AS (ACTIVE[percpu_index()])
#define FPU_HOLDS_PROCESS (FPU_HOLDS[percpu_index()])

extern volatile int SCHED_LOCKED;
extern volatile unsigned SCHED_OWNER;

static inline void sched_lock(void)
{
    unsigned me = percpu_index() + 1;
    while (__atomic_exchange_n(&SCHED_LOCKED, 1, __ATOMIC_ACQUIRE))
    {
        if (SCHED_OWNER == me)
            __builtin_trap(); /* this processor holds it already: waiting would be for ever */
#if defined(__x86_64__)
        __asm__ volatile("pause");
#endif
    }
    SCHED_OWNER = me;
}

static inline void sched_unlock(void)
{
    SCHED_OWNER = 0;
    __atomic_store_n(&SCHED_LOCKED, 0, __ATOMIC_RELEASE);
}

/* ---- types, constants and variables used by more than one file ---- */
/* What task_ctx_switch pops, lowest address first: the saved xmm6-15, the eight pushed registers (the first one
 * popped is r15) and the return address it finally "returns" to. */
typedef struct
{
    unsigned long long xmm[20];
    unsigned long long r15, r14, r13, r12, rsi, rdi, rbx, rbp;
    unsigned long long ret;
} TaskInitFrame;

/* ---- task_core.c ---- */
extern TaskChunk *DIR[TASK_LIMIT / TASK_CHUNK];
extern int CAP;
extern int CURS[SMP_MAX_CPUS];
extern unsigned SLICES[SMP_MAX_CPUS];
extern AddressSpace *ACTIVE[SMP_MAX_CPUS];
extern int FPU_HOLDS[SMP_MAX_CPUS];
extern int IDLE_ID[SMP_MAX_CPUS];
extern int AP_READY;
extern int BOOT_RESERVED;
extern unsigned long long SWITCHES;
extern unsigned long long CPU_TICKS_ALL[SMP_MAX_CPUS];
extern unsigned long long CPU_TICKS_BUSY[SMP_MAX_CPUS];
extern unsigned long long TICKS;
extern volatile int SCHED_LOCKED;
extern volatile unsigned SCHED_OWNER;

/* ---- task_sched.c ---- */
int eligible(int k, int cpu);
unsigned slice_of(int k);
int better_waiting(void);
void fill_zero(void *p, unsigned long long bytes);
void fpu_enter(const Task *t);
void reap(void);
void schedule_locked(void);
void schedule(void);
void task_main(void);

/* ---- task_create.c ---- */
int add_chunk(void);

/* ---- task_run.c ---- */
void kick_idle_cpus(void);

#endif
