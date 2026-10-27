/* Kernel threads: the scheduler (ranking, slices, reaping, switching). */
#include "task_internal.h"

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
 * processor), and never the main loop. The idle context of a processor is never a candidate: it is what runs when there is none.
 * Two refinements. A thread with an affinity runs only on that processor (an application processor only if the thread is a user thread:
 * a kernel thread would hold the big kernel lock the whole time and stop the boot processor's main loop). With the boot processor
 * reserved, the user threads leave it as soon as an application processor is there to take them, so it keeps to the desktop. */
int eligible(int k, int cpu)
{
    const Task *t = tk(k);
    if (t->idle_cpu)
        return 0;
    if (t->affinity >= 0)
        return cpu == t->affinity && (cpu == 0 || t->as != 0);
    if (cpu == 0)
        return !(BOOT_RESERVED && AP_READY > 0 && t->as != 0);
    return t->as != 0;
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
unsigned slice_of(int k)
{
    const Task *t = tk(k);
    return task_slice_for(t->inherit > t->prio ? t->inherit : t->prio);
}

/* 1 if another thread is READY (or asleep with its time come) and ranks strictly higher than the running one. It is how a wake-up
 * of a more important thread takes effect: the timer tick checks this and does not wait for the end of the slice. Interrupts off. */
int better_waiting(void)
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


void fill_zero(void *p, unsigned long long bytes)
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
void fpu_enter(const Task *t)
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
 * current thread is skipped (it cannot be DEAD here anyway). Interrupts must be off, the scheduler lock not held.
 * Each dead thread is detached under the lock (its resources copied out, its slot made free), and the resources are given back after
 * the lock is let go: free_data and aspace_destroy may call any task function. The slot can be reused at once; nothing in it is left. */
void reap(void)
{
    for (;;)
    {
        AddressSpace *as = 0;
        void *data = 0;
        TaskFreeFn free_data = 0;
        unsigned char *stack = 0;
        int guarded = 0, found = 0;

        sched_lock();
        for (int i = 0; i < CAP && !found; i++)
        {
            Task *t = tk(i);
            if (t->state == TASK_DEAD && !running_somewhere(i))
            {
                found = 1;
                as = t->as; /* the process's pages and tables. Its space is not loaded: schedule() switched to another one. */
                data = t->data;
                free_data = t->free_data;
                stack = t->stack;
                guarded = t->guarded;
                t->as = 0;
                t->data = 0;
                t->free_data = 0;
                t->stack = 0;
                t->guarded = 0;
                t->state = TASK_UNUSED;
            }
        }
        sched_unlock();
        if (!found)
            return;

        if (as)
            aspace_destroy(as);
        if (data && free_data)
            free_data(data);
        if (guarded)
            paging_set_present((unsigned long long)stack, 1); /* the page goes back to the allocator: map it again */
        if (stack)
            pmm_free_contiguous((unsigned long long)stack, STACK_BLOCK_PAGES);
    }
}

/* Choose the next ready thread after the current one (round robin) and switch to it. Returns when the calling
 * thread is chosen again. Interrupts must be off, the big kernel lock held (see SCHED_LOCKED above), the scheduler lock held on entry: it
 * is let go before this returns (by whichever thread comes out of the switch: see the lock's comment) and is NOT held on return. */
void schedule_locked(void)
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
        sched_unlock();
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
    tk(next)->last_cpu = me;
    CUR = next;
    SWITCHES++;
    SLICE_LEFT = slice_of(next); /* the thread that comes in gets a whole slice */
    task_ctx_switch(&old->rsp, tk(next)->rsp);
    sched_unlock(); /* the switch is complete: the thread that left has saved its stack pointer. (Taken by the processor we run on now.) */
    fpu_enter(tk(CUR)); /* we are the thread that was chosen to continue: its floating point state now */
    reap();
}

void schedule(void)
{
    sched_lock();
    schedule_locked();
}

/* The first code of every new thread: task_ctx_switch "returns" here, with interrupts off (the switch ran inside
 * schedule()) and the scheduler lock held by the processor that switched to us. */
void task_main(void)
{
    sched_unlock();
    fpu_enter(tk(CUR)); /* a new thread never returns from schedule(), so it takes its floating point state here */
    reap(); /* a thread that came here instead of returning from schedule() must free dead threads too */
    if (tk(CUR)->flags & TASK_PREEMPTIBLE)
        irq_enable(); /* from here on the timer may take the CPU away */
    tk(CUR)->fn(tk(CUR)->arg);
    task_exit();
}

/* A new chunk of TASK_CHUNK free slots, if the table may still grow. The heap locks itself (heap.c); the table itself is only changed with interrupts off (DIR first, then CAP). 1 = there are more slots
 * now (possibly made by someone else meanwhile), 0 = at TASK_LIMIT or out of memory. */
