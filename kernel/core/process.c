/* HarisOS user processes: see include/process.h. */
#include "process.h"
#include "aspace.h"
#include "heap.h"
#include "idle.h"
#include "pmm.h"
#include "segments.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"

typedef unsigned long long u64;

static ProcessStats STATS;

void process_stats(ProcessStats *out) { *out = STATS; }

/* Leave ring 0 for ring 3 with IRETQ: it pops RIP, CS, RFLAGS, RSP, SS, so build that frame and go. RFLAGS = 0x202:
 * interrupts on (the timer must be able to take the CPU from a process that never gives it up), bit 1 is always set.
 * The general registers are cleared so no kernel value is handed to user code. Arguments: rcx = entry, rdx = user RSP. */
#if defined(__x86_64__) && !defined(HARIS_HOST)

__attribute__((noreturn)) void process_enter_user(u64 entry, u64 user_rsp);

__asm__(".text\n"
        ".p2align 4\n"
        ".globl process_enter_user\n"
        "process_enter_user:\n"
        "    pushq $0x1B\n" /* SS = SEL_USER_DATA */
        "    pushq %rdx\n"
        "    pushq $0x202\n"
        "    pushq $0x23\n" /* CS = SEL_USER_CODE */
        "    pushq %rcx\n"
        "    xorl %eax, %eax\n"
        "    xorl %ebx, %ebx\n"
        "    xorl %ecx, %ecx\n"
        "    xorl %edx, %edx\n"
        "    xorl %esi, %esi\n"
        "    xorl %edi, %edi\n"
        "    xorl %ebp, %ebp\n"
        "    xorl %r8d, %r8d\n"
        "    xorl %r9d, %r9d\n"
        "    xorl %r10d, %r10d\n"
        "    xorl %r11d, %r11d\n"
        "    xorl %r12d, %r12d\n"
        "    xorl %r13d, %r13d\n"
        "    xorl %r14d, %r14d\n"
        "    xorl %r15d, %r15d\n"
        "    iretq\n");

_Static_assert(SEL_USER_DATA == 0x1B && SEL_USER_CODE == 0x23, "process_enter_user pushes these selectors as numbers");

#else

__attribute__((noreturn)) static void process_enter_user(u64 entry, u64 user_rsp)
{
    (void)entry;
    (void)user_rsp;
    for (;;)
        ;
}

#endif

typedef struct
{
    u64 entry, user_rsp;
} Start;

/* The thread's first code. The scheduler has already loaded the process's address space and set the kernel stack that
 * ring 3 traps will land on; all that is left is to go to ring 3. */
static void user_thread_main(void *arg)
{
    Start st = *(Start *)arg;
    kfree(arg);
    process_enter_user(st.entry, st.user_rsp);
}

/* Copy bytes into a user page from the kernel: through the page's physical address (RAM is identity mapped), so
 * the target space need not be loaded. volatile: no memcpy in a freestanding build. */
static int copy_in(AddressSpace *as, u64 va, const unsigned char *src, u64 n)
{
    for (u64 done = 0; done < n;)
    {
        u64 phys;
        if (aspace_translate(as, va + done, &phys) != 0)
            return -1;
        u64 chunk = 4096 - (phys & 4095);
        if (chunk > n - done)
            chunk = n - done;
        volatile unsigned char *dst = (volatile unsigned char *)phys;
        for (u64 i = 0; i < chunk; i++)
            dst[i] = src[done + i];
        done += chunk;
    }
    return 0;
}

int process_start(const char *name, const void *image, u64 size)
{
    if (!image || size == 0 || size > PROCESS_IMAGE_MAX)
        return -1;
    AddressSpace *as = aspace_create();
    if (!as)
        return -1;

    /* Code and data of the image: executable, not writable. The stack: writable, not executable (W^X). */
    u64 pages = (size + 4095) / 4096;
    for (u64 i = 0; i < pages; i++)
        if (aspace_map_new(as, PAGING_USER_BASE + i * 4096, ASPACE_EXEC) != 0)
            goto fail;
    for (int i = 1; i <= PROCESS_STACK_PAGES; i++)
        if (aspace_map_new(as, PAGING_USER_END - (u64)i * 4096, ASPACE_WRITE) != 0)
            goto fail;
    if (copy_in(as, PAGING_USER_BASE, (const unsigned char *)image, size) != 0)
        goto fail;

    Start *st = kmalloc(sizeof(Start));
    if (!st)
        goto fail;
    st->entry = PAGING_USER_BASE;
    st->user_rsp = PAGING_USER_END - 16; /* inside the top stack page; 16-aligned */
    int id = task_create_user(name, user_thread_main, st, as);
    if (id < 0)
    {
        kfree(st);
        goto fail;
    }
    STATS.started++;
    return id;

fail:
    aspace_destroy(as);
    return -1;
}

__attribute__((noreturn)) void process_exit(int code)
{
    STATS.exited++;
    STATS.last_exit = code;
    serial_puts("HARISOS: process ");
    serial_puts(task_name(task_current()));
    serial_puts(" exited, code ");
    serial_putd(code);
    serial_puts("\n");
    task_exit(); /* the scheduler frees the thread, its pages and its page tables */
}

__attribute__((noreturn)) void process_fault(const FaultFrame *f, u64 cr2)
{
    STATS.faulted++;
    static char text[1024]; /* not on the stack: this runs on a small emergency stack for page faults */
    fault_format(text, sizeof(text), f, cr2);
    serial_puts("HARISOS: process ");
    serial_puts(task_name(task_current()));
    serial_puts(" ended by a CPU exception in ring 3:\n");
    serial_puts(text);
    task_exit();
}

#if defined(__x86_64__) && !defined(HARIS_HOST)

extern const unsigned char user_hello_start[], user_hello_end[];
extern const unsigned char user_badptr_start[], user_badptr_end[];
extern const unsigned char user_fault_start[], user_fault_end[];
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

/* Start a program and give it the CPU until its thread has ended, then let the scheduler clean up. */
static int run(const char *name, const unsigned char *start, const unsigned char *end)
{
    int id = process_start(name, start, (u64)(end - start));
    if (id < 0)
        return -1;
    for (int i = 0; i < 200; i++)
    {
        TaskState s = task_state(id);
        if (s == TASK_DEAD || s == TASK_UNUSED)
            break;
        task_yield(); /* it runs until it traps, is preempted by the timer, or ends */
    }
    task_yield(); /* frees the stack, the pages and the page tables of the finished thread */
    task_yield();
    return task_state(id) == TASK_UNUSED ? 0 : -2;
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
