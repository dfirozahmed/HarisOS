/* Kernel threads: the table, creating and killing threads. */
#include "task_internal.h"

int add_chunk(void)
{
    if (CAP >= TASK_LIMIT)
        return 0;
    TaskChunk *c = kmalloc(sizeof(TaskChunk)); /* the heap has a lock of its own (heap.c) */
    if (!c)
        return 0;
    fill_zero(c, sizeof(*c)); /* every slot TASK_UNUSED, nothing held */
    for (int i = 0; i < TASK_CHUNK; i++)
    {
        c->t[i].prio = TASK_PRIO_DEFAULT;
        c->t[i].inherit = -1;
        c->t[i].affinity = -1;
    }
    unsigned long long irq = irq_save();
    sched_lock();
    int idx = CAP / TASK_CHUNK;
    int taken = DIR[idx] != 0;
    if (!taken)
    {
        DIR[idx] = c;
        __atomic_store_n(&CAP, CAP + TASK_CHUNK, __ATOMIC_RELEASE); /* after DIR: whoever reads CAP may use the new slots */
    }
    sched_unlock();
    irq_restore(irq);
    if (taken) /* another create was faster: give ours back */
    {
        kfree(c);
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
        tk(i)->run_ticks = 0;
        tk(i)->last_cpu = i == 0 ? 0 : -1;
        tk(i)->idle_cpu = 0;
        tk(i)->kill_pending = 0;
        tk(i)->affinity = -1;
    }
    AP_READY = 0;
    BOOT_RESERVED = 0;
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
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD && !tk(id)->kill_pending ? tk(id)->data : 0;
}

void task_exec_replace(AddressSpace *as, const char *name)
{
    Task *t = tk(CUR);
    AddressSpace *old = t->as;
    unsigned long long irq = irq_save();
    sched_lock(); /* `as` decides which processors may run this thread (eligible) */
    t->as = as;
    sched_unlock();
    irq_restore(irq);
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
    sched_lock();
    if (tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
    {
        sched_unlock();
        irq_restore(irq);
        return -1;
    }
    if (tk(id)->state == TASK_RUNNING)
    {
        /* Running on another processor, so it is in ring 3 (we hold the big kernel lock: nobody else is in the kernel). It cannot be
         * stopped from here; its processor ends it at its next timer tick (task_tick), when it is in the kernel again. */
        tk(id)->kill_pending = 1;
        sched_unlock();
        irq_restore(irq);
        return 0;
    }
    tk(id)->state = TASK_DEAD; /* reap() (run by the next thread that runs) frees it */
    sched_unlock();
    irq_restore(irq);
    return 0;
}

/* Blocking: a thread that waits for something (a key, a message) must not be READY, or the scheduler would keep running
 * it just to look again. It marks itself BLOCKED and gives up the CPU; whoever produces the event calls task_wake().
 * Interrupts are off from the check of the condition (the caller's) to here only if the caller keeps them off, as
 * system calls do: nothing can wake the thread between its check and its block. */
static int create(const char *name, TaskFn fn, void *arg, unsigned flags, AddressSpace *as, void *data,
                  TaskFreeFn free_data)
{
    if (!fn)
        return -1;
    unsigned char *stack = (unsigned char *)pmm_alloc_contiguous_tag(STACK_BLOCK_PAGES, PMM_TAG_SYSTEM); /* (the page allocator locks itself) */
    if (!stack)
        return -1;
    CANARY_AT(stack) = STACK_CANARY;

    unsigned long long irq;
    int id;
    for (;;)
    {
        irq = irq_save(); /* the slot search and the fill below must not be interleaved with another create */
        reap();
        sched_lock(); /* (held to the end: nobody else can take the slot we found) */
        id = -1;
        for (int i = 1; i < CAP; i++) /* slot 0 is the main thread */
            if (tk(i)->state == TASK_UNUSED)
            {
                id = i;
                break;
            }
        if (id >= 0)
            break; /* (interrupts stay off from here to the end) */
        sched_unlock();
        irq_restore(irq);
        if (!add_chunk()) /* every slot is in use: make room, or give up */
        {
            pmm_free_contiguous((unsigned long long)stack, STACK_BLOCK_PAGES);
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
    t->run_ticks = 0;
    t->last_cpu = -1;
    t->kill_pending = 0;
    t->affinity = -1; /* (a slot used before may have been pinned) */
    int n = 0;
    for (; name && name[n] && n < TASK_NAME_MAX - 1; n++)
        t->name[n] = name[n];
    t->name[n] = 0;
    t->state = TASK_READY; /* last: from now on the scheduler may pick it */
    sched_unlock();
    kick_idle_cpus();
    irq_restore(irq);
    return id;
}
