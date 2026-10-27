/* Processes: exit records, reaping, signals, process list, exit and faults. */
#include "process_internal.h"

#define ZOMBIE_MAX 65536
static struct Zombie
{
    int pid, code;
    int owner; /* pid of the parent */
} *ZOMBIE;
static int Z_CAP, Z_USED, Z_NEXT; /* entries, entries that may be in use (the rest is free), where to overwrite at the limit */

void purge_exits(int owner)
{
    for (int i = 0; i < Z_USED; i++)
        if (ZOMBIE[i].pid != 0 && ZOMBIE[i].owner == owner)
            ZOMBIE[i].pid = 0;
    while (Z_USED > 0 && ZOMBIE[Z_USED - 1].pid == 0)
        Z_USED--;
}

static void record_exit(const Process *p, int code)
{
    if (!p || p->parent < 0)
        return;
    const Process *pp = task_data_of(p->parent);
    if (!pp) /* the parent is gone: nobody could collect the code, and nothing would ever purge the record */
        return;
    int slot = -1;
    for (int i = 0; i < Z_USED && slot < 0; i++)
        if (ZOMBIE[i].pid == 0)
            slot = i;
    if (slot < 0 && Z_USED == Z_CAP && Z_CAP < ZOMBIE_MAX)
    {
        int cap = Z_CAP ? Z_CAP * 2 : 32;
        struct Zombie *n = krealloc(ZOMBIE, (u64)cap * sizeof(struct Zombie));
        if (n)
        {
            ZOMBIE = n;
            Z_CAP = cap;
        }
    }
    if (slot < 0 && Z_USED < Z_CAP)
        slot = Z_USED++;
    if (slot < 0 && Z_USED > 0) /* no room and no memory: the oldest record goes */
        slot = Z_NEXT++ % Z_USED;
    if (slot >= 0)
    {
        ZOMBIE[slot].pid = p->pid;
        ZOMBIE[slot].code = code;
        ZOMBIE[slot].owner = pp->pid;
    }
    task_wake(p->parent); /* if the parent has ended and the slot belongs to another thread, that is a harmless extra wake */
}

int process_reap(int pid, int *code)
{
    if (pid <= 0)
        return 0;
    for (int i = 0; i < Z_USED; i++)
        if (ZOMBIE[i].pid == pid)
        {
            if (code)
                *code = ZOMBIE[i].code;
            ZOMBIE[i].pid = 0;
            while (Z_USED > 0 && ZOMBIE[Z_USED - 1].pid == 0)
                Z_USED--;
            return 1;
        }
    return 0;
}

/* ---- signals --------------------------------------------------------------------------------------------------------- */
/* Ends the process of thread `tid` (not the running one) as if it had called SYS_EXIT(code): the same bookkeeping as
 * process_exit, then the thread is marked DEAD and the next reap frees it. It was either in ring 3 (preempted by the timer) or
 * blocked inside a system call; neither holds anything in the kernel. */
static void terminate(int tid, int sig)
{
    Process *p = task_data_of(tid);
    if (!p)
        return;
    int code = 128 + sig;
    STATS.killed++;
    task_set_exit_code(tid, code);
    serial_puts("HARISOS: process ");
    serial_puts(task_name(tid));
    serial_puts(" ended by signal ");
    serial_putd(sig);
    serial_puts("\n");
    if (p->own_console)
        console_finish(p->console, code); /* "[process exited, code 130]" in its window */
    process_release_files(p);
    record_exit(p, code);
    task_kill(tid);
}

/* one process: delivered unless ignored (SIGKILL cannot be) */
static void deliver(int tid, int sig)
{
    Process *p = task_data_of(tid);
    if (!p)
        return;
    if (sig != SIGKILL && (p->sig_ignore & (1u << sig)))
        return;
    if (sig != SIGKILL && sig < 32 && p->sig_handler[sig] && p->sig_entry)
    {
        p->sig_pending |= 1u << sig; /* its handler runs when it is about to return to ring 3 (usersig.h); a sleeping system call is woken */
        task_wake(tid);
        return;
    }
    if (tid == task_current())
    {
        STATS.killed++;
        process_exit(128 + sig); /* does not return */
    }
    terminate(tid, sig);
}

int process_signal(int pid, int sig)
{
    if (pid <= 0 || sig < 0 || sig > 31)
        return -1;
    for (int tid = 1; tid < task_slots(); tid++)
    {
        const Process *p = task_data_of(tid);
        if (p && p->pid == pid)
        {
            if (sig != 0)
                deliver(tid, sig);
            return 0;
        }
    }
    return -1;
}

int process_priority(int pid, int prio)
{
    if (pid <= 0 || prio < -1 || prio > TASK_PRIO_MAX)
        return -1;
    for (int tid = 1; tid < task_slots(); tid++)
    {
        const Process *p = task_data_of(tid);
        if (p && p->pid == pid)
        {
            int old = task_priority(tid);
            if (old < 0 || (prio >= 0 && task_set_priority(tid, prio) != 0))
                return -1;
            return old;
        }
    }
    return -1;
}

void process_interrupt_console(int con)
{
    if (con < 0)
        return;
    for (int tid = 1; tid < task_slots(); tid++)
    {
        const Process *p = task_data_of(tid);
        if (p && p->console == con)
            deliver(tid, SIGINT);
    }
}

int process_info(int index, ProcessInfo *out)
{
    int seen = 0;
    for (int tid = 1; tid < task_slots(); tid++)
    {
        const Process *p = task_data_of(tid);
        if (!p)
            continue;
        if (seen++ != index)
            continue;
        out->pid = p->pid;
        out->ppid = 0;
        if (p->parent > 0)
        {
            const Process *pp = task_data_of(p->parent);
            out->ppid = pp ? pp->pid : 0;
        }
        TaskState st = task_state(tid);
        out->running = st == TASK_READY || st == TASK_RUNNING;
        out->console = p->console;
        const char *name = task_name(tid);
        int i = 0;
        for (; name && name[i] && i < (int)sizeof(out->name) - 1; i++)
            out->name[i] = name[i];
        out->name[i] = 0;
        return 1;
    }
    return 0;
}

__attribute__((noreturn)) void process_exit(int code)
{
    STATS.exited++;
    STATS.last_exit = code;
    task_set_exit_code(task_current(), code);
    serial_puts("HARISOS: process ");
    serial_puts(task_name(task_current()));
    serial_puts(" exited, code ");
    serial_putd(code);
    serial_puts("\n");
    Process *p = process_current();
    if (p && p->own_console)
        console_finish(p->console, code); /* "[process exited, code N]" in its window (does nothing without one) */
    process_release_files(p); /* a reader of its pipes now sees end of input */
    record_exit(p, code);
    task_exit(); /* the scheduler frees the thread, its pages and its page tables */
}

static void hex64(char *out, u64 v) /* 16 hex digits and a NUL */
{
    for (int i = 0; i < 16; i++)
        out[i] = "0123456789ABCDEF"[(v >> (60 - 4 * i)) & 15];
    out[16] = 0;
}

__attribute__((noreturn)) void process_fault(const FaultFrame *f, u64 cr2)
{
    STATS.faulted++;
    task_set_exit_code(task_current(), -1);
    static char text[1024]; /* not on the stack: this runs on a small emergency stack for page faults */
    fault_format(text, sizeof(text), f, cr2);
    serial_puts("HARISOS: process ");
    serial_puts(task_name(task_current()));
    serial_puts(" ended by a CPU exception in ring 3:\n");
    serial_puts(text);

    Process *p = process_current();
    if (p && console_valid(p->console)) /* tell the user in the program's window what happened, and where */
    {
        static char line[96];
        const char *name = fault_name((unsigned)f->vector);
        int n = 0;
        static const char head[] = "\n[process crashed: ";
        for (int i = 0; head[i]; i++)
            line[n++] = head[i];
        for (int i = 0; name[i] && n < 50; i++)
            line[n++] = name[i];
        static const char at[] = " at ";
        for (int i = 0; at[i]; i++)
            line[n++] = at[i];
        hex64(line + n, f->rip);
        n += 16;
        line[n++] = ']';
        line[n++] = '\n';
        console_write(p->console, line, (unsigned)n);
        if (p->own_console)
            console_finish(p->console, -1);
    }
    process_release_files(p);
    record_exit(p, -1);
    task_exit();
}
