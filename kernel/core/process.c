/* HarisOS user processes: see include/process.h. */
#include "process.h"
#include "aspace.h"
#include "console.h"
#include "elf.h"
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

static int EXIT_CODE_OF[TASK_MAX]; /* by thread id */

int process_exit_code(int id) { return id >= 0 && id < TASK_MAX ? EXIT_CODE_OF[id] : 0; }

int process_wait(int id)
{
    for (int i = 0; i < 400; i++)
    {
        TaskState s = task_state(id);
        if (s == TASK_DEAD || s == TASK_UNUSED)
            break;
        task_yield(); /* it runs until it traps, is preempted by the timer, or ends */
    }
    task_yield(); /* frees the stack, the pages and the page tables of the finished thread */
    task_yield();
    return task_state(id) == TASK_UNUSED ? 0 : -1;
}

/* Leave ring 0 for ring 3 with IRETQ: it pops RIP, CS, RFLAGS, RSP, SS, so build that frame and go. RFLAGS = 0x202:
 * interrupts on (the timer must be able to take the CPU from a process that never gives it up), bit 1 is always set.
 * The general registers are cleared so no kernel value is handed to user code. Arguments: rcx = entry, rdx = user RSP, r8 = argc, r9 = argv (they arrive in rcx, rdx and in rdi, rsi: Microsoft and System V calling conventions). */
#if defined(__x86_64__) && !defined(HARIS_HOST)

__attribute__((noreturn)) void process_enter_user(u64 entry, u64 user_rsp, u64 argc, u64 argv);

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
        "    movq %r8, %rcx\n" /* argc, argv for a program built with the Microsoft x64 convention (the user programs are) */
        "    movq %r9, %rdx\n"
        "    movq %r9, %rsi\n" /* and for the System V convention */
        "    movq %r8, %rdi\n"
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

__attribute__((noreturn)) static void process_enter_user(u64 entry, u64 user_rsp, u64 argc, u64 argv)
{
    (void)entry;
    (void)user_rsp;
    (void)argc;
    (void)argv;
    for (;;)
        ;
}

#endif

typedef struct
{
    u64 entry, user_rsp, argc, argv;
} Start;

/* The thread's first code. The scheduler has already loaded the process's address space and set the kernel stack that
 * ring 3 traps will land on; all that is left is to go to ring 3. */
static void user_thread_main(void *arg)
{
    Start st = *(Start *)arg;
    kfree(arg);
    process_enter_user(st.entry, st.user_rsp, st.argc, st.argv);
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

/* The last steps of every way to start a process: the stack (writable, not executable), the first thread data, the
 * thread. On failure the address space is destroyed here, so the callers just return. Returns the thread id or -1. */
static void process_free(void *p) { kfree(p); }

static int NEXT_PID = 1;

/* The arguments go to the top of the stack, then the argv array below them, then the (fake) return address:
 *
 *   PAGING_USER_END   -> +--------------------+
 *                        | "arg0\0arg1\0..."  |   the strings, as given
 *                        | argv[0..argc-1]    |   pointers to them, then NULL (16-aligned start)
 *   user_rsp          -> | 0 (return address) |   so rsp is 8 mod 16 at the first instruction
 *
 * The blob is checked here as well (exactly argc strings, all inside args_len): it is built by the kernel, but a mistake
 * must not become a write outside the stack. Returns 0, or -1 (nothing is written then). */
static int place_args(AddressSpace *as, const ProcessSpawn *sp, Start *st)
{
    int argc = sp->argc;
    unsigned len = sp->args_len;
    if (argc < 0 || argc > PROCESS_ARGS_MAX || len > PROCESS_ARGS_BYTES || (argc > 0 && (!sp->args || len == 0)))
        return -1;
    u64 argv_ptr[PROCESS_ARGS_MAX + 1];
    unsigned off = 0;
    u64 str_base = PAGING_USER_END - len;
    for (int i = 0; i < argc; i++)
    {
        if (off >= len)
            return -1;
        argv_ptr[i] = str_base + off;
        while (off < len && sp->args[off])
            off++;
        if (off >= len)
            return -1; /* a string without its NUL */
        off++;
    }
    if (off != len)
        return -1; /* bytes left over */
    argv_ptr[argc] = 0;
    u64 array_bytes = (u64)(argc + 1) * 8;
    u64 array_base = (str_base - array_bytes) & ~15ULL;
    if (len && copy_in(as, str_base, (const unsigned char *)sp->args, len) != 0)
        return -1;
    if (copy_in(as, array_base, (const unsigned char *)argv_ptr, array_bytes) != 0)
        return -1;
    st->argc = (u64)argc;
    st->argv = array_base;
    st->user_rsp = array_base - 8;
    return 0;
}

static int launch(const char *name, AddressSpace *as, u64 entry, ProcessSpawn *sp, u64 heap_base)
{
    for (int i = 1; i <= PROCESS_STACK_PAGES; i++)
        if (aspace_map_new(as, PAGING_USER_END - (u64)i * 4096, ASPACE_WRITE) != 0)
        {
            aspace_destroy(as);
            return -1;
        }
    Start *st = kmalloc(sizeof(Start));
    Process *p = kmalloc(sizeof(Process));
    if (!st || !p)
    {
        kfree(st);
        kfree(p);
        aspace_destroy(as);
        return -1;
    }
    st->entry = entry;
    /* The stack pointer is 8 mod 16 at the first instruction, exactly as if the program had been called (a call pushes an
     * 8-byte return address onto a 16-aligned stack). The C calling convention relies on it: a compiler that has to save
     * xmm6-xmm15 in a function's prologue stores them with aligned moves relative to that assumption, and a stack that is
     * 16-aligned instead faults on the first one. The slot at [rsp] is the "return address": zero, never used.
     * place_args puts the arguments above it and moves user_rsp to match. */
    st->user_rsp = PAGING_USER_END - 8;
    st->argc = st->argv = 0;
    if (place_args(as, sp, st) != 0)
    {
        kfree(st);
        kfree(p);
        aspace_destroy(as);
        return -1;
    }
    p->console = sp->console;
    p->own_console = sp->parent < 0;
    p->pid = NEXT_PID;
    p->parent = sp->parent;
    p->cwd = sp->cwd;
    for (int i = 0; i < PROCESS_MAX_CHILDREN; i++)
        p->child[i] = 0;
    /* The heap may not run into the stack: it stops below the 1 MiB the stack may grow into (ELF_STACK_RESERVE). */
    u64 stack_low = PAGING_USER_END - 1024ULL * 1024;
    p->heap_base = p->heap_brk = p->heap_mapped = heap_base;
    p->heap_limit = heap_base + PROCESS_HEAP_MAX < stack_low ? heap_base + PROCESS_HEAP_MAX : stack_low;
    if (p->heap_limit < heap_base)
        p->heap_limit = heap_base; /* the program sits that high: no heap */
    for (int i = 0; i < PROCESS_MAX_FILES; i++)
    {
        p->file[i].node = -1;
        p->file[i].pos = 0;
        p->file[i].flags = 0;
    }
    for (int k = 0; k < 3; k++)
        p->stdio[k] = -1;
    if (sp->redirect) /* shell redirection: files stand in for fd 0, 1, 2 (more than PROCESS_MAX_FILES cannot happen: only 3) */
    {
        int used = 0;
        for (int k = 0; k < 3; k++)
        {
            const ProcessFile *r = &sp->redirect[k];
            if (r->node < 0)
                continue;
            int slot = -1;
            for (int j = 0; j < k; j++) /* the same file and mode as an earlier one: share it */
                if (sp->redirect[j].node == r->node && sp->redirect[j].flags == r->flags && p->stdio[j] >= 0)
                    slot = p->stdio[j];
            if (slot < 0)
                slot = used++;
            p->file[slot] = *r;
            p->stdio[k] = slot;
        }
    }
    /* the Process record belongs to the thread from here on: process_free is called when the thread is cleaned up */
    int id = task_create_user(name, user_thread_main, st, as, p, process_free);
    if (id < 0)
    {
        kfree(st);
        kfree(p);
        aspace_destroy(as);
        return -1;
    }
    STATS.started++;
    sp->pid = NEXT_PID++;
    return id;
}

Process *process_current(void) { return (Process *)task_user_data(); }

int process_start(const char *name, const void *image, u64 size)
{
    if (!image || size == 0 || size > PROCESS_IMAGE_MAX)
        return -1;
    AddressSpace *as = aspace_create();
    if (!as)
        return -1;

    /* Code and data of the image: executable, not writable (W^X). */
    u64 pages = (size + 4095) / 4096;
    for (u64 i = 0; i < pages; i++)
        if (aspace_map_new(as, PAGING_USER_BASE + i * 4096, ASPACE_EXEC) != 0)
        {
            aspace_destroy(as);
            return -1;
        }
    if (copy_in(as, PAGING_USER_BASE, (const unsigned char *)image, size) != 0)
    {
        aspace_destroy(as);
        return -1;
    }
    ProcessSpawn sp = {.console = -1, .parent = -1};
    return launch(name, as, PAGING_USER_BASE, &sp, PAGING_USER_BASE + pages * 4096);
}

int process_start_elf(const char *name, const void *file, u64 size, int console, int *err)
{
    ProcessSpawn sp = {.console = console, .parent = -1};
    return process_spawn_elf(name, file, size, &sp, err);
}

int process_spawn_elf(const char *name, const void *file, u64 size, ProcessSpawn *sp, int *err)
{
    ElfInfo info;
    int e = elf_parse(file, size, &info); /* every field of the file is checked here, before anything is mapped */
    if (err)
        *err = e;
    if (e != ELF_OK)
        return -1;
    AddressSpace *as = aspace_create();
    if (!as)
        return -1;

    /* Each segment gets its own pages (elf_parse made sure no page is shared) with the permissions it asks for. A fresh
     * page is zero, which is exactly what the bytes after filesz (.bss) must be; only the file's bytes are copied. */
    u64 heap_base = 0; /* the first page after the last segment */
    for (int i = 0; i < info.nseg; i++)
    {
        const ElfSegment *s = &info.seg[i];
        u64 seg_end = (s->vaddr + s->memsz + 4095) & ~4095ULL;
        if (seg_end > heap_base)
            heap_base = seg_end;
        unsigned flags = ((s->flags & ELF_PF_W) ? ASPACE_WRITE : 0) | ((s->flags & ELF_PF_X) ? ASPACE_EXEC : 0);
        u64 first = s->vaddr & ~4095ULL, last = (s->vaddr + s->memsz - 1) & ~4095ULL;
        for (u64 va = first; va <= last; va += 4096)
            if (aspace_map_new(as, va, flags) != 0)
            {
                aspace_destroy(as);
                return -1;
            }
        if (copy_in(as, s->vaddr, (const unsigned char *)file + s->offset, s->filesz) != 0)
        {
            aspace_destroy(as);
            return -1;
        }
    }
    return launch(name, as, info.entry, sp, heap_base);
}

/* The exit codes of children that their parent has not collected yet (a ring: the oldest record is overwritten). The pid
 * is never reused, so a stale record can never be mistaken for a newer process. Only children are recorded. */
#define ZOMBIES 32
static struct
{
    int pid, code;
} ZOMBIE[ZOMBIES];
static unsigned ZOMBIE_NEXT;

static void record_exit(const Process *p, int code)
{
    if (!p || p->parent < 0)
        return;
    ZOMBIE[ZOMBIE_NEXT % ZOMBIES].pid = p->pid;
    ZOMBIE[ZOMBIE_NEXT % ZOMBIES].code = code;
    ZOMBIE_NEXT++;
    task_wake(p->parent); /* if the parent has ended and the slot belongs to another thread, that is a harmless extra wake */
}

int process_reap(int pid, int *code)
{
    if (pid <= 0)
        return 0;
    for (int i = 0; i < ZOMBIES; i++)
        if (ZOMBIE[i].pid == pid)
        {
            if (code)
                *code = ZOMBIE[i].code;
            ZOMBIE[i].pid = 0;
            return 1;
        }
    return 0;
}

__attribute__((noreturn)) void process_exit(int code)
{
    STATS.exited++;
    STATS.last_exit = code;
    EXIT_CODE_OF[task_current()] = code;
    serial_puts("HARISOS: process ");
    serial_puts(task_name(task_current()));
    serial_puts(" exited, code ");
    serial_putd(code);
    serial_puts("\n");
    Process *p = process_current();
    if (p && p->own_console)
        console_finish(p->console, code); /* "[process exited, code N]" in its window (does nothing without one) */
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
    EXIT_CODE_OF[task_current()] = -1;
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
    record_exit(p, -1);
    task_exit();
}

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
