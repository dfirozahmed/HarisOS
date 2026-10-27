/* Processes: ring 3 boot self tests and the SMP spin test. */
#include "process_internal.h"

#if defined(__x86_64__) && !defined(HARIS_HOST)

extern const unsigned char user_hello_start[], user_hello_end[];
extern const unsigned char user_badptr_start[], user_badptr_end[];
extern const unsigned char user_fault_start[], user_fault_end[];
extern const unsigned char user_segs_start[], user_segs_end[];
extern const unsigned char user_spin_start[], user_spin_end[], user_spin_stop[], user_spin_deadline[];

static u64 rdtsc(void)
{
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* Write a 64-bit word into a process's page from the kernel (through the physical address). */
static void poke64(AddressSpace *as, u64 va, u64 value)
{
    u64 phys;
    if (aspace_translate(as, va, &phys) == 0)
        *(volatile u64 *)phys = value;
}

/* The timer interrupt must take the CPU away from ring 3 code that never calls the kernel. The program spins; we give
 * it the CPU over and over, and every time we get it back while it is still alive, the timer did that. After three
 * times it is told to stop. It also stops by itself at a deadline, so a broken timer fails the test, not the boot. */
static int spin_test(void)
{
    unsigned khz = timer_tsc_khz();
    if (!khz || !idle_ready())
        return 0; /* no clock to measure a deadline with, or no timer tick: nothing to test */
    int id = process_start("SPIN", user_spin_start, (u64)(user_spin_end - user_spin_start));
    AddressSpace *as = id >= 0 ? task_space_of(id) : 0;
    if (!as)
        return -1;
    poke64(as, PAGING_USER_BASE + (u64)(user_spin_deadline - user_spin_start), rdtsc() + 500ULL * khz); /* 500 ms */

    int preempted = 0;
    for (int i = 0; i < 400; i++)
    {
        TaskState s = task_state(id);
        if (s == TASK_DEAD || s == TASK_UNUSED)
            break;
        task_yield();
        s = task_state(id);
        if (s != TASK_DEAD && s != TASK_UNUSED && ++preempted == 3)
            poke64(as, PAGING_USER_BASE + (u64)(user_spin_stop - user_spin_start), 1);
    }
    task_yield();
    task_yield();
    if (task_state(id) != TASK_UNUSED)
        return -2;
    if (STATS.last_exit == 6)
        return -3; /* it ran until its deadline: the timer never took the CPU from it */
    return (STATS.last_exit == 5 && preempted >= 3) ? 0 : -4;
}

/* The Task Manager's per-processor button: a SPIN program pinned to application processor `cpu`, which keeps it busy until
 * process_spin_stop (or SPIN_MAX_MS, so a forgotten one does not run for ever). The thread is READY before the pin is set, so it may
 * get a slice somewhere else first; after its next switch only `cpu` takes it. */
#define SPIN_MAX_MS 600000ULL
int process_spin_start(int cpu)
{
    unsigned khz = timer_tsc_khz();
    if (cpu < 1 || !khz || !idle_ready())
        return -1;
    int id = process_start("SPIN", user_spin_start, (u64)(user_spin_end - user_spin_start));
    AddressSpace *as = id >= 0 ? task_space_of(id) : 0;
    if (!as)
        return -1;
    poke64(as, PAGING_USER_BASE + (u64)(user_spin_deadline - user_spin_start), rdtsc() + SPIN_MAX_MS * khz);
    if (task_set_affinity(id, cpu) != 0)
    {
        poke64(as, PAGING_USER_BASE + (u64)(user_spin_stop - user_spin_start), 1); /* no such processor: let it end */
        return -1;
    }
    return id;
}

void process_spin_stop(int id)
{
    AddressSpace *as = task_space_of(id);
    const char *name = task_name(id);
    if (as && name && name[0] == 'S' && name[1] == 'P' && name[2] == 'I' && name[3] == 'N' && name[4] == 0)
        poke64(as, PAGING_USER_BASE + (u64)(user_spin_stop - user_spin_start), 1);
}

/* Start a program and give it the CPU until its thread has ended, then let the scheduler clean up. */
static int run(const char *name, const unsigned char *start, const unsigned char *end)
{
    int id = process_start(name, start, (u64)(end - start));
    if (id < 0)
        return -1;
    return process_wait(id) == 0 ? 0 : -2;
}

int process_selftest(void)
{
    u64 free0 = pmm_free_pages();
    ProcessStats s0 = STATS;
    u64 calls0 = syscall_count();

    /* a program that writes a line and exits with code 7: two system calls, normal exit */
    if (run("HELLO", user_hello_start, user_hello_end) != 0)
        return -1;
    if (STATS.exited != s0.exited + 1 || STATS.last_exit != 7 || syscall_count() != calls0 + 2)
        return -2;

    /* a pointer into the kernel: the write is refused, the program sees -1 and exits with it */
    if (run("BADPTR", user_badptr_start, user_badptr_end) != 0)
        return -3;
    if (STATS.exited != s0.exited + 2 || STATS.last_exit != -1)
        return -4;

    /* touching kernel memory from ring 3: only that process dies, the kernel goes on */
    if (run("FAULT", user_fault_start, user_fault_end) != 0)
        return -5;
    if (STATS.faulted != s0.faulted + 1)
        return -6;

    /* the selectors ring 3 runs with after SYSRET: SS = 0x1B, CS = 0x23, both with RPL 3 (see syscall_init) */
    if (run("SEGS", user_segs_start, user_segs_end) != 0)
        return -15;
    if (STATS.last_exit != (int)((0x1BU << 16) | 0x23U))
        return -16;

    int sp = spin_test();
    if (sp != 0)
        return -10 + sp; /* -11 .. -14 */

    if (pmm_free_pages() != free0)
        return -7; /* a page, a stack or a table was not given back */
    return 0;
}

#else

int process_selftest(void) { return -1; }

#endif
