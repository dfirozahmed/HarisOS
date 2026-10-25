/* HarisOS kernel threads (cooperative): see include/task.h. */
#include "task.h"
#include "aspace.h"
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
    unsigned cpus_ran;      /* bit n: this thread has been switched in on processor n (task_cpu_mask) */
    int idle_cpu;           /* the idle context of application processor n-1 (an entry of task_register_idle), 0 = an ordinary thread */
    int kill_pending;       /* task_kill found it running on another processor: it ends at that processor's next timer tick */
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
static TaskChunk *DIR[TASK_LIMIT / TASK_CHUNK];
static int CAP;

static inline Task *tk(int id) { return &DIR[id / TASK_CHUNK]->t[id % TASK_CHUNK]; } /* id < CAP */
/* Processors (smp.h). All of this is kernel code under the big kernel lock (bkl.h), so one scheduler with one table serves every processor;
 * what belongs to a processor is kept per processor: the running thread, its slice, the address space loaded in its CR3 and whether its
 * floating point unit holds a process's registers. CUR & co. below are those, for the processor this code runs on. A thread can move to
 * another processor whenever it is switched out, so these must be read again after task_ctx_switch returns (the macros do, being
 * re-evaluated at every use). Processor 0 is the boot processor and runs the main loop (thread 0). An application processor runs only
 * user threads and, when there is none, its own idle context (task_register_idle). */
static int CURS[SMP_MAX_CPUS];
static unsigned SLICES[SMP_MAX_CPUS];
static AddressSpace *ACTIVE[SMP_MAX_CPUS];
static int FPU_HOLDS[SMP_MAX_CPUS];
static int IDLE_ID[SMP_MAX_CPUS]; /* the idle context of application processor n, 0 = none yet */
#define CUR (CURS[percpu_index()])
#define SLICE_LEFT (SLICES[percpu_index()])
#define ACTIVE_AS (ACTIVE[percpu_index()])
#define FPU_HOLDS_PROCESS (FPU_HOLDS[percpu_index()])
static unsigned long long SWITCHES;
static unsigned long long TICKS; /* timer interrupts seen by task_tick, for the boot log */
volatile int preempt_count; /* see preempt.h */

/* The priority the scheduler ranks thread k by: its own or the one it borrowed (the higher), plus one level per TASK_AGE_PASSES
 * passes it waited. */
/* How many passes a READY thread may be passed over before it gets a boost: TASK_AGE_PASSES, or the number of threads if that is more, so
 * that plain round robin stays exact among threads of equal priority however many there are. */
static unsigned age_passes(void) { return (unsigned)CAP > TASK_AGE_PASSES ? (unsigned)CAP : TASK_AGE_PASSES; }

static int rank_of(int k)
{
    const Task *t = tk(k);
    return (t->inherit > t->prio ? t->inherit : t->prio) + (int)(t->waited / age_passes());
}

/* The slice grows with the priority: the default level gets TASK_SLICE_TICKS, level 0 a third of it (at least one tick), level
 * TASK_PRIO_MAX a bit over one and a half times. A thread that is wanted more also gets to keep the CPU longer. */
unsigned task_slice_for(int prio)
{
    if (prio < TASK_PRIO_MIN)
        prio = TASK_PRIO_MIN;
    if (prio > TASK_PRIO_MAX)
        prio = TASK_PRIO_MAX;
    unsigned n = (unsigned)(TASK_SLICE_TICKS * (prio + 1)) / (TASK_PRIO_DEFAULT + 1);
    return n ? n : 1;
}

/* May processor `cpu` run thread k? The boot processor runs everything but the idle contexts of the others; an application processor only
 * user threads (their ring 3 code is what runs in parallel: kernel threads drive hardware and the GUI, which belong to the boot
 * processor), and never the main loop. The idle context of a processor is never a candidate: it is what runs when there is none. */
static int eligible(int k, int cpu)
{
    const Task *t = tk(k);
    if (t->idle_cpu)
        return 0;
    return cpu == 0 || t->as != 0;
}

/* 1 if thread k is the running thread of some processor. */
static int running_somewhere(int k)
{
    if (k == 0)
        return 1; /* the main thread runs on the boot processor for ever */
    for (int c = 1; c < SMP_MAX_CPUS; c++)
        if (CURS[c] == k && IDLE_ID[c] != 0) /* (an unused entry of CURS is 0 and means nothing) */
            return 1;
    return CURS[0] == k;
}

/* The slice the running thread (or the one that is about to run) gets, by the priority it runs at (borrowed one included). */
static unsigned slice_of(int k)
{
    const Task *t = tk(k);
    return task_slice_for(t->inherit > t->prio ? t->inherit : t->prio);
}

/* 1 if another thread is READY (or asleep with its time come) and ranks strictly higher than the running one. It is how a wake-up
 * of a more important thread takes effect: the timer tick checks this and does not wait for the end of the slice. Interrupts off. */
static int better_waiting(void)
{
    unsigned long long now = timer_us();
    int mine = rank_of(CUR);
    int me = (int)percpu_index();
    for (int i = 0; i < CAP; i++)
    {
        if (i == CUR || !eligible(i, me))
            continue;
        const Task *t = tk(i);
        if ((t->state == TASK_READY || (t->state == TASK_SLEEPING && (long long)(now - t->wake_us) >= 0)) && rank_of(i) > mine)
            return 1;
    }
    return 0;
}

/* What task_ctx_switch pops, lowest address first: the saved xmm6-15, the eight pushed registers (the first one
 * popped is r15) and the return address it finally "returns" to. */
typedef struct
{
    unsigned long long xmm[20];
    unsigned long long r15, r14, r13, r12, rsi, rdi, rbx, rbp;
    unsigned long long ret;
} TaskInitFrame;

static void fill_zero(void *p, unsigned long long bytes)
{
    volatile unsigned char *b = (volatile unsigned char *)p; /* volatile: no call to memset in a freestanding build */
    for (unsigned long long i = 0; i < bytes; i++)
        b[i] = 0;
}

/* True while the x87 unit and MXCSR hold the registers of a process. Then a kernel thread that gets the CPU must first put
 * them back to the normal values (the kernel's own SSE code expects the default rounding mode and masked exceptions). Per processor:
 * FPU_HOLDS_PROCESS is a macro near the top of the file. */

/* The running thread `t` has just got the CPU: give it the floating point state it expects.
 *   process thread  -> its own saved registers (or the clean state, for a new one): all of FXSAVE, i.e. x87, MXCSR, xmm.
 *   kernel thread   -> if a process had the unit: reset the x87 unit and MXCSR, but NOT the xmm registers. A kernel thread
 *                      that called schedule() expects xmm6-xmm15 back as it left them (callee-saved), and task_ctx_switch
 *                      has just restored exactly that; a full restore of a default image would overwrite them. */
static void fpu_enter(const Task *t)
{
    if (t->as)
    {
        fpu_restore(t->fpu);
        FPU_HOLDS_PROCESS = 1;
    }
    else if (FPU_HOLDS_PROCESS)
    {
        fpu_reset_control();
        FPU_HOLDS_PROCESS = 0;
    }
}

/* Frees the stacks of finished threads. Only safe from another thread's stack, never from the dying one: the
 * current thread is skipped (it cannot be DEAD here anyway). Interrupts must be off. */
static void reap(void)
{
    for (int i = 0; i < CAP; i++)
    {
        Task *t = tk(i);
        if (t->state == TASK_DEAD && !running_somewhere(i))
        {
            if (t->as) /* the process's pages and tables. Its space is not loaded: schedule() switched to another one. */
            {
                aspace_destroy(t->as);
                t->as = 0;
            }
            if (t->data && t->free_data)
                t->free_data(t->data);
            t->data = 0;
            t->free_data = 0;
            if (t->guarded)
                paging_set_present((unsigned long long)t->stack, 1); /* the page goes back to the allocator: map it again */
            pmm_free_contiguous((unsigned long long)t->stack, STACK_BLOCK_PAGES);
            t->stack = 0;
            t->guarded = 0;
            t->state = TASK_UNUSED;
        }
    }
}

/* Choose the next ready thread after the current one (round robin) and switch to it. Returns when the calling
 * thread is chosen again. Interrupts must be off. */
static void schedule(void)
{
    Task *old = tk(CUR);
    unsigned long long now = timer_us();

    for (int i = 0; i < CAP; i++)
        if (tk(i)->state == TASK_SLEEPING && (long long)(now - tk(i)->wake_us) >= 0)
            tk(i)->state = TASK_READY;
    if (old->state == TASK_RUNNING)
        old->state = TASK_READY;

    /* Highest effective priority wins. The scan starts after the current thread and only a strictly better one replaces the
     * candidate, so equal threads keep their round robin order (and the current thread, last in the scan, loses ties). */
    int me = (int)percpu_index();
    int next = -1, best = 0;
    for (int i = 1; i <= CAP; i++) /* i == CAP comes back to the current thread */
    {
        int k = (CUR + i) % CAP;
        if (tk(k)->state == TASK_READY && eligible(k, me))
        {
            int eff = rank_of(k);
            if (next < 0 || eff > best)
            {
                next = k;
                best = eff;
            }
        }
    }
    if (next < 0 && me > 0 && IDLE_ID[me])
        next = IDLE_ID[me]; /* an application processor with nothing to run: its idle context (the caller of task_cpu_idle) */
    if (next < 0)
        __builtin_trap(); /* cannot happen: task 0 never sleeps or ends, so it is always ready or running */

    /* Aging: everyone left waiting counts the pass (capped, so the counter cannot overflow); the winner starts over. */
    for (int i = 0; i < CAP; i++)
        if (i != next && tk(i)->state == TASK_READY && tk(i)->waited < age_passes() * (TASK_PRIO_MAX + 1))
            tk(i)->waited++;
    tk(next)->waited = 0;

    if (next == CUR)
    {
        old->state = TASK_RUNNING;
        reap(); /* nobody else to run, but a thread that just ended can be cleaned up */
        return;
    }
    if (old->stack && CANARY_AT(old->stack) != STACK_CANARY)
        __builtin_trap(); /* the leaving thread overflowed its stack: shows up as #UD on the crash screen */

    /* The next thread runs in its own address space (kernel threads in the kernel's). The kernel half is the same in
     * all of them, so this code and the stack we are on stay valid across the switch. Ring 3 code of the next thread
     * will trap onto the next thread's kernel stack. */
    if (tk(next)->as != ACTIVE_AS)
    {
        aspace_switch(tk(next)->as);
        ACTIVE_AS = tk(next)->as;
    }
    if (tk(next)->as)
        syscall_set_kernel_stack(tk(next)->kstack_top);

    /* Floating point state belongs to the process. The one that leaves saves it here, before the switch; the one that comes
     * in loads it after the switch, in its own context (fpu_enter), not here: task_ctx_switch itself saves and restores
     * xmm6-xmm15 of the thread that calls it, and loading the next thread's registers before that would make it store the
     * wrong values for the one that leaves. (xmm0-xmm5 are not part of this: whoever interrupted the process, the timer
     * interrupt or a system call, saved them in its own frame.) */
    if (old->as)
        fpu_save(old->fpu);

    tk(next)->state = TASK_RUNNING;
    tk(next)->cpus_ran |= 1u << me;
    CUR = next;
    SWITCHES++;
    SLICE_LEFT = slice_of(next); /* the thread that comes in gets a whole slice */
    task_ctx_switch(&old->rsp, tk(next)->rsp);
    fpu_enter(tk(CUR)); /* we are the thread that was chosen to continue: its floating point state now */
    reap();
}

/* The first code of every new thread: task_ctx_switch "returns" here, with interrupts off (the switch ran inside
 * schedule()). */
static void task_main(void)
{
    fpu_enter(tk(CUR)); /* a new thread never returns from schedule(), so it takes its floating point state here */
    reap(); /* a thread that came here instead of returning from schedule() must free dead threads too */
    if (tk(CUR)->flags & TASK_PREEMPTIBLE)
        irq_enable(); /* from here on the timer may take the CPU away */
    tk(CUR)->fn(tk(CUR)->arg);
    task_exit();
}

/* A new chunk of TASK_CHUNK free slots, if the table may still grow. The heap is not safe against the timer interrupt, so the block is
 * taken with preemption off, and the table itself is only changed with interrupts off (DIR first, then CAP). 1 = there are more slots
 * now (possibly made by someone else meanwhile), 0 = at TASK_LIMIT or out of memory. */
static int add_chunk(void)
{
    if (CAP >= TASK_LIMIT)
        return 0;
    preempt_disable();
    TaskChunk *c = kmalloc(sizeof(TaskChunk));
    preempt_enable();
    if (!c)
        return 0;
    fill_zero(c, sizeof(*c)); /* every slot TASK_UNUSED, nothing held */
    for (int i = 0; i < TASK_CHUNK; i++)
    {
        c->t[i].prio = TASK_PRIO_DEFAULT;
        c->t[i].inherit = -1;
    }
    unsigned long long irq = irq_save();
    int idx = CAP / TASK_CHUNK;
    int taken = DIR[idx] != 0;
    if (!taken)
    {
        DIR[idx] = c;
        CAP += TASK_CHUNK;
    }
    irq_restore(irq);
    if (taken) /* another create was faster: give ours back */
    {
        preempt_disable();
        kfree(c);
        preempt_enable();
    }
    return 1;
}

int task_init(void)
{
    if (CAP == 0 && !add_chunk()) /* the first chunk holds the main thread */
        return -1;
    for (int i = 0; i < CAP; i++)
    {
        tk(i)->state = TASK_UNUSED;
        tk(i)->stack = 0;
        tk(i)->guarded = 0;
        tk(i)->as = 0;
        tk(i)->data = 0;
        tk(i)->free_data = 0;
        tk(i)->flags = 0;
        tk(i)->prio = TASK_PRIO_DEFAULT;
        tk(i)->inherit = -1;
        tk(i)->waited = 0;
        tk(i)->held = 0;
        tk(i)->blocked = 0;
        tk(i)->exit_code = 0;
        tk(i)->cpus_ran = 0;
        tk(i)->idle_cpu = 0;
        tk(i)->kill_pending = 0;
    }
    for (int c = 0; c < SMP_MAX_CPUS; c++)
    {
        CURS[c] = 0;
        IDLE_ID[c] = 0;
        ACTIVE[c] = 0;
        FPU_HOLDS[c] = 0;
        SLICES[c] = TASK_SLICE_TICKS;
    }
    ACTIVE_AS = 0;
    FPU_HOLDS_PROCESS = 0;
    SLICE_LEFT = TASK_SLICE_TICKS;
    preempt_count = 0;
    Task *t = tk(0);
    t->state = TASK_RUNNING;
    t->name[0] = 'M';
    t->name[1] = 'A';
    t->name[2] = 'I';
    t->name[3] = 'N';
    t->name[4] = 0;
    CUR = 0;
    SWITCHES = 0;
    return 0;
}

static int create(const char *name, TaskFn fn, void *arg, unsigned flags, AddressSpace *as, void *data,
                  TaskFreeFn free_data);

int task_create(const char *name, TaskFn fn, void *arg) { return create(name, fn, arg, 0, 0, 0, 0); }

int task_create_flags(const char *name, TaskFn fn, void *arg, unsigned flags)
{
    return create(name, fn, arg, flags, 0, 0, 0);
}

/* A process thread must be preemptible: its ring 3 code never yields by itself. */
int task_create_user(const char *name, TaskFn fn, void *arg, AddressSpace *as, void *data, TaskFreeFn free_data)
{
    return as ? create(name, fn, arg, TASK_PREEMPTIBLE, as, data, free_data) : -1;
}

AddressSpace *task_address_space(void) { return tk(CUR)->as; }

void *task_user_data(void) { return tk(CUR)->data; }

void *task_data_of(int id)
{
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD ? tk(id)->data : 0;
}

void task_exec_replace(AddressSpace *as, const char *name)
{
    Task *t = tk(CUR);
    AddressSpace *old = t->as;
    t->as = as;
    aspace_switch(as); /* the kernel half is the same in both: this code and its stack stay valid */
    ACTIVE_AS = as;
    aspace_destroy(old); /* no longer loaded */
    fpu_default(t->fpu);
    fpu_enter(t);
    int n = 0;
    for (; name && name[n] && n < TASK_NAME_MAX - 1; n++)
        t->name[n] = name[n];
    t->name[n] = 0;
}

int task_kill(int id)
{
    if (id <= 0 || id >= CAP || id == CUR || tk(id)->idle_cpu)
        return -1;
    unsigned long long irq = irq_save();
    if (tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
    {
        irq_restore(irq);
        return -1;
    }
    if (tk(id)->state == TASK_RUNNING)
    {
        /* Running on another processor, so it is in ring 3 (we hold the big kernel lock: nobody else is in the kernel). It cannot be
         * stopped from here; its processor ends it at its next timer tick (task_tick), when it is in the kernel again. */
        tk(id)->kill_pending = 1;
        irq_restore(irq);
        return 0;
    }
    tk(id)->state = TASK_DEAD; /* reap() (run by the next thread that runs) frees it */
    irq_restore(irq);
    return 0;
}

/* Blocking: a thread that waits for something (a key, a message) must not be READY, or the scheduler would keep running
 * it just to look again. It marks itself BLOCKED and gives up the CPU; whoever produces the event calls task_wake().
 * Interrupts are off from the check of the condition (the caller's) to here only if the caller keeps them off, as
 * system calls do: nothing can wake the thread between its check and its block. */
void task_block(void)
{
    if (CUR == 0) /* the main loop never blocks: it is what runs when everybody else does */
    {
        task_yield();
        return;
    }
    unsigned long long irq = irq_save();
    tk(CUR)->state = TASK_BLOCKED;
    schedule();
    irq_restore(irq);
}

void task_wake(int id)
{
    if (id < 0 || id >= CAP)
        return;
    unsigned long long irq = irq_save();
    if (tk(id)->state == TASK_BLOCKED)
        tk(id)->state = TASK_READY;
    irq_restore(irq);
}

AddressSpace *task_space_of(int id)
{
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD ? tk(id)->as : 0;
}

static int create(const char *name, TaskFn fn, void *arg, unsigned flags, AddressSpace *as, void *data,
                  TaskFreeFn free_data)
{
    if (!fn)
        return -1;
    /* The page allocator has no lock of its own: keep the timer from switching to a thread that uses it too. */
    preempt_disable();
    unsigned char *stack = (unsigned char *)pmm_alloc_contiguous_tag(STACK_BLOCK_PAGES, PMM_TAG_SYSTEM);
    preempt_enable();
    if (!stack)
        return -1;
    CANARY_AT(stack) = STACK_CANARY;

    unsigned long long irq;
    int id;
    for (;;)
    {
        irq = irq_save(); /* the slot search and the fill below must not be interleaved with another create */
        reap();
        id = -1;
        for (int i = 1; i < CAP; i++) /* slot 0 is the main thread */
            if (tk(i)->state == TASK_UNUSED)
            {
                id = i;
                break;
            }
        if (id >= 0)
            break; /* (interrupts stay off from here to the end) */
        irq_restore(irq);
        if (!add_chunk()) /* every slot is in use: make room, or give up */
        {
            preempt_disable();
            pmm_free_contiguous((unsigned long long)stack, STACK_BLOCK_PAGES);
            preempt_enable();
            return -1;
        }
    }
    int guarded = paging_set_present((unsigned long long)stack, 0) == 0; /* if it fails the stack just has no guard */

    /* Layout at the top of the stack (top is 16-aligned), highest address first:
     *   top-8 .. top-40  32 bytes of "shadow space" the Microsoft ABI gives every callee above its return address
     *   top-40           fake return address of task_main (it never returns: it ends in task_exit)
     *   top-48           ret slot of the frame below = task_main's address
     * task_main starts as if it had been called: RSP = top-40, which is 8 mod 16 like at any function entry. */
    unsigned long long top = ((unsigned long long)stack + STACK_BLOCK_PAGES * PMM_PAGE_SIZE) & ~15ULL;
    unsigned long long *ret_slot = (unsigned long long *)(top - 48);
    TaskInitFrame *fr = (TaskInitFrame *)((unsigned char *)ret_slot - __builtin_offsetof(TaskInitFrame, ret));
    fill_zero(fr, (unsigned long long)((unsigned char *)(top) - (unsigned char *)fr));
    fr->ret = (unsigned long long)task_main;

    Task *t = tk(id);
    t->stack = stack;
    t->guarded = guarded;
    t->as = as; /* set before the state says READY: the scheduler never sees a process thread without its space */
    t->data = data;
    t->free_data = free_data;
    t->kstack_top = top;
    if (as)
        fpu_default(t->fpu); /* a new process starts with clean floating point registers */
    t->rsp = (unsigned long long)fr;
    t->fn = fn;
    t->arg = arg;
    t->flags = flags;
    t->prio = TASK_PRIO_DEFAULT;
    t->inherit = -1;
    t->waited = 0;
    t->wake_us = 0;
    t->held = 0; /* a new thread holds no mutex and sleeps on none, whatever the thread of this slot before it left behind */
    t->blocked = 0;
    t->exit_code = 0;
    t->cpus_ran = 0;
    t->idle_cpu = 0;
    t->kill_pending = 0;
    int n = 0;
    for (; name && name[n] && n < TASK_NAME_MAX - 1; n++)
        t->name[n] = name[n];
    t->name[n] = 0;
    t->state = TASK_READY; /* last: from now on the scheduler may pick it */
    irq_restore(irq);
    return id;
}

void task_yield(void)
{
    unsigned long long irq = irq_save();
    schedule();
    irq_restore(irq);
}

int task_set_priority(int id, int prio)
{
    if (id < 0 || id >= CAP || prio < TASK_PRIO_MIN || prio > TASK_PRIO_MAX)
        return -1;
    unsigned long long irq = irq_save();
    if (tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
    {
        irq_restore(irq);
        return -1;
    }
    tk(id)->prio = prio;
    tk(id)->waited = 0; /* an old boost belonged to the old level */
    irq_restore(irq);
    return 0;
}

int task_priority(int id)
{
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD ? tk(id)->prio : -1;
}

int task_set_inherited(int id, int prio)
{
    if (id < 0 || id >= CAP || prio < -1 || prio > TASK_PRIO_MAX)
        return -1;
    unsigned long long irq = irq_save();
    if (tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
    {
        irq_restore(irq);
        return -1;
    }
    tk(id)->inherit = prio;
    irq_restore(irq);
    return 0;
}

int task_effective_priority(int id)
{
    if (id < 0 || id >= CAP || tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
        return -1;
    return tk(id)->inherit > tk(id)->prio ? tk(id)->inherit : tk(id)->prio;
}

void task_sleep_ms(unsigned ms)
{
    if (CUR == 0) /* the main loop never sleeps: it is what runs when everybody else does */
    {
        task_yield();
        return;
    }
    unsigned long long irq = irq_save();
    tk(CUR)->wake_us = timer_us() + (unsigned long long)ms * 1000;
    tk(CUR)->state = TASK_SLEEPING;
    schedule();
    irq_restore(irq);
}

__attribute__((noreturn)) void task_exit(void)
{
    if (CUR == 0)
        __builtin_trap(); /* ending the main loop would leave nothing to run */
    irq_save(); /* never restored: this thread does not continue */
    tk(CUR)->state = TASK_DEAD;
    schedule(); /* never comes back: a dead thread is never chosen */
    for (;;)
        __builtin_trap();
}

void task_tick(void)
{
    TICKS++;
    Task *t = tk(CUR);
    if (t->kill_pending && CUR != 0)
    {
        /* task_kill found this thread running on this processor. It was in ring 3 then, so it holds nothing: end it here. The interrupt
         * frame on its stack is thrown away with the rest of the thread. */
        t->kill_pending = 0;
        t->state = TASK_DEAD;
        schedule(); /* never comes back: a dead thread is never chosen */
        return;
    }
    if (!(t->flags & TASK_PREEMPTIBLE) || t->state != TASK_RUNNING || preempt_count > 0)
        return;
    /* A thread that matters more became ready (task_wake, a sleep that ended, a priority that was raised): it takes the CPU now, not
     * when the slice ends. This is the only safe place for it: task_wake can be called from code that must not be switched away from
     * (it holds interrupts off, a pipe, the console), so it only makes the thread READY and this tick, at most one millisecond later,
     * does the switch. A running thread that is not preemptible never gets here (the return above). */
    if (!better_waiting() && --SLICE_LEFT > 0)
        return;
    SLICE_LEFT = slice_of(CUR);
    schedule(); /* we are in the interrupt: interrupts are off */
}

int task_others_ready(void)
{
    unsigned long long irq = irq_save();
    unsigned long long now = timer_us();
    int found = 0;
    int me = (int)percpu_index();
    for (int i = 0; i < CAP && !found; i++)
    {
        if (i == CUR || !eligible(i, me))
            continue;
        if (tk(i)->state == TASK_READY ||
            (tk(i)->state == TASK_SLEEPING && (long long)(now - tk(i)->wake_us) >= 0))
            found = 1;
    }
    irq_restore(irq);
    return found;
}

int task_current(void) { return CUR; }

int task_slots(void) { return CAP; }

void **task_held_slot(int id) { return id >= 0 && id < CAP ? &tk(id)->held : 0; }

void **task_blocked_slot(int id) { return id >= 0 && id < CAP ? &tk(id)->blocked : 0; }

void task_set_exit_code(int id, int code)
{
    if (id >= 0 && id < CAP)
        tk(id)->exit_code = code;
}

int task_exit_code(int id) { return id >= 0 && id < CAP ? tk(id)->exit_code : 0; }

int task_count(void)
{
    int n = 0;
    for (int i = 0; i < CAP; i++)
        if (tk(i)->state != TASK_UNUSED && tk(i)->state != TASK_DEAD && !tk(i)->idle_cpu)
            n++;
    return n;
}

/* The idle contexts of the application processors are not threads to the outside: they do not show up in the lists. */
TaskState task_state(int id) { return id >= 0 && id < CAP && !tk(id)->idle_cpu ? tk(id)->state : TASK_UNUSED; }

const char *task_name(int id)
{
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && !tk(id)->idle_cpu ? tk(id)->name : 0;
}

unsigned task_cpu_mask(int id) { return id >= 0 && id < CAP ? tk(id)->cpus_ran : 0; }

/* The calling application processor (its idle loop in smp.c, in the kernel with the big kernel lock, interrupts off) becomes a context the
 * scheduler can switch to and from: a slot of its own that is never a candidate for the others. 0, or -1 (not an application processor,
 * already done, no slot). */
int task_register_idle(void)
{
    int cpu = (int)percpu_index();
    if (cpu < 1 || cpu >= SMP_MAX_CPUS || IDLE_ID[cpu])
        return -1;
    for (;;)
    {
        unsigned long long irq = irq_save();
        for (int i = 1; i < CAP; i++)
            if (tk(i)->state == TASK_UNUSED)
            {
                Task *t = tk(i);
                t->stack = 0;
                t->guarded = 0;
                t->as = 0;
                t->data = 0;
                t->free_data = 0;
                t->flags = 0;
                t->prio = TASK_PRIO_DEFAULT;
                t->inherit = -1;
                t->waited = 0;
                t->held = 0;
                t->blocked = 0;
                t->exit_code = 0;
                t->cpus_ran = 1u << cpu;
                t->kill_pending = 0;
                t->idle_cpu = cpu;
                t->name[0] = 'I';
                t->name[1] = 'D';
                t->name[2] = 'L';
                t->name[3] = 'E';
                t->name[4] = (char)('0' + cpu % 10);
                t->name[5] = 0;
                t->state = TASK_RUNNING; /* the calling processor is running it right now */
                IDLE_ID[cpu] = i;
                CURS[cpu] = i;
                irq_restore(irq);
                return 0;
            }
        irq_restore(irq);
        if (!add_chunk())
            return -1;
    }
}

/* The idle loop of an application processor asks: is there a user thread for me? It runs it (schedule() switches away and comes back
 * here, in the idle context, when the processor has nothing to run again). Kernel, big kernel lock held, interrupts off. */
void task_cpu_idle(void)
{
    int cpu = (int)percpu_index();
    if (cpu < 1 || !IDLE_ID[cpu])
        return;
    schedule();
}

unsigned long long task_switches(void) { return SWITCHES; }

unsigned long long task_ticks(void) { return TICKS; }

const char *task_guard_hit(unsigned long long addr)
{
    for (int i = 0; i < CAP; i++)
    {
        const Task *t = tk(i);
        unsigned long long base = (unsigned long long)t->stack;
        if (t->guarded && t->state != TASK_UNUSED && addr >= base && addr < base + PMM_PAGE_SIZE)
            return t->name;
    }
    return 0;
}

/* ---- boot self test ----------------------------------------- */
static void selftest_worker(void *arg)
{
    volatile int *counter = arg;
    for (int i = 0; i < 3; i++)
    {
        (*counter)++;
        task_yield();
    }
}

int task_selftest(void)
{
    volatile int a = 0, b = 0;
    int ta = task_create("TEST A", selftest_worker, (void *)&a);
    int tb = task_create("TEST B", selftest_worker, (void *)&b);
    if (ta < 0 || tb < 0)
        return -1;

    task_yield(); /* A runs once and yields, B runs once and yields, back to us: they take turns */
    if (a != 1 || b != 1)
        return -2;

    for (int i = 0; i < 20 && task_count() > 1; i++)
        task_yield();
    if (a != 3 || b != 3 || task_count() != 1)
        return -3;
    task_yield(); /* the last dead thread's stack is freed by the next switch */
    if (task_state(ta) != TASK_UNUSED || task_state(tb) != TASK_UNUSED)
        return -4;
    return 0;
}

/* A thread that never yields. Only the timer interrupt can take the CPU from it. It stops when the test tells it to
 * (SPIN_STOP), or at a generous deadline, so a broken tick makes the test fail instead of hanging the boot. The test
 * does not depend on how fast the machine is: it waits for a number of preemptions, not for a number of milliseconds
 * (an emulator on a busy host can be very slow). */
#define SPIN_NEEDED 3        /* preemptions that prove the timer works */
#define SPIN_DEADLINE_US 500000
static volatile unsigned long long SPIN_COUNT;
static volatile int SPIN_DONE, SPIN_STOP;
static unsigned long long SPIN_DEADLINE;

static void spin_worker(void *arg)
{
    (void)arg;
    while (!SPIN_STOP && (long long)(timer_us() - SPIN_DEADLINE) < 0)
        SPIN_COUNT++;
    SPIN_DONE = 1;
}

int task_selftest_preempt(void)
{
    if (!idle_ready() || !timer_ready())
        return 0; /* no tick, so there is nothing to preempt with: threads stay cooperative */

    SPIN_COUNT = 0;
    SPIN_DONE = 0;
    SPIN_STOP = 0;
    SPIN_DEADLINE = timer_us() + SPIN_DEADLINE_US;
    if (task_create_flags("SPIN", spin_worker, 0, TASK_PREEMPTIBLE) < 0)
        return -1;

    int preempted = 0;
    for (int i = 0; i < 400 && !SPIN_DONE; i++)
    {
        task_yield(); /* the spinner runs; we are back because its slice ended (timer) or because it finished */
        if (!SPIN_DONE && ++preempted >= SPIN_NEEDED)
            SPIN_STOP = 1; /* enough proof: let it end at its next turn */
    }
    task_yield(); /* frees the stack of the finished spinner */
    if (!SPIN_DONE)
        return -2;
    if (preempted < SPIN_NEEDED) /* it ran to its deadline: the timer never took the CPU from it */
        return -3;
    if (SPIN_COUNT == 0)
        return -4;
    return 0;
}
