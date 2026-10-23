/* HarisOS system calls: see include/syscall.h. */
#include "syscall.h"
#include "aspace.h"
#include "console.h"
#include "exec.h"
#include "gdt.h"
#include "process.h"
#include "serial.h"
#include "task.h"
#include "uaccess.h"
#include "vfs.h"

typedef unsigned long long ull;

static ull COUNT;

ull syscall_count(void) { return COUNT; }

/* ---- moving bytes between ring 3 and the kernel ------------------------------------------------------------------
 * Every pointer from ring 3 is checked with aspace_user_ok() before it is used (mapped, user accessible, writable if it
 * is to be written) and only then touched, inside the SMAP window (uaccess.h) and in small pieces through a buffer on the
 * kernel stack: code that talks to the console, the serial port or a drive never runs with the window open. Interrupts
 * are off for the whole call, so the pages cannot be unmapped between the check and the copy, and the process's own
 * address space is the one loaded, so its pointers can be used as they are. */
#define CHUNK 512

static void copy_from_user(char *dst, ull src, unsigned n)
{
    const volatile char *p = (const volatile char *)src; /* volatile: no memcpy in this build, and it must really read */
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        dst[i] = p[i];
    user_access_end();
}

static void copy_to_user(ull dst, const char *src, unsigned n)
{
    volatile char *p = (volatile char *)dst;
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        p[i] = src[i];
    user_access_end();
}

/* A NUL-terminated string from ring 3 into dst[cap]: its length, or -1 if it is not readable user memory or does not fit.
 * The pages are checked one at a time as the string runs into them (its length is not known in advance). */
static int copy_string_from_user(char *dst, int cap, ull src, const AddressSpace *as)
{
    for (int i = 0; i < cap; i++)
    {
        ull a = src + (ull)i;
        if ((i == 0 || (a & 4095) == 0) && !aspace_user_ok(as, a, 1, 0))
            return -1;
        char c;
        copy_from_user(&c, a, 1);
        dst[i] = c;
        if (c == 0)
            return i;
    }
    return -1;
}

/* ---- the calls ----------------------------------------------------------------------------------------------------- */
static long long sys_write(const SyscallFrame *f)
{
    ull fd = f->rdi, ptr = f->rsi, len = f->rdx;
    if (fd != 1 && fd != 2) /* standard output and standard error: the console */
        return -1;
    if (len > SYS_WRITE_MAX)
        return -1;
    if (len == 0)
        return 0;
    const AddressSpace *as = task_address_space();
    if (!as || !aspace_user_ok(as, ptr, len, 0))
        return -1;
    const Process *p = process_current();
    char buf[CHUNK];
    for (ull done = 0; done < len;)
    {
        unsigned n = (len - done) < CHUNK ? (unsigned)(len - done) : CHUNK;
        copy_from_user(buf, ptr + done, n);
        if (p && p->console >= 0)
            console_write(p->console, buf, n); /* (a window that was closed takes nothing: the text is dropped) */
        else
            for (unsigned i = 0; i < n; i++) /* no window: the serial port, where a program started at boot talks */
            {
                if (buf[i] == '\n')
                    serial_putc('\r');
                serial_putc(buf[i]);
            }
        done += n;
    }
    return (long long)len;
}

static long long sys_read(const SyscallFrame *f)
{
    ull fd = f->rdi, ptr = f->rsi, len = f->rdx;
    if (len == 0)
        return 0;
    if (len > SYS_READ_MAX)
        len = SYS_READ_MAX; /* a read may return less than asked */
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, ptr, len, 1))
        return -1;
    char buf[CHUNK];

    if (fd == 0) /* standard input: a line typed in the console window */
    {
        if (p->console < 0)
            return 0; /* no window: no input */
        int want = len < CHUNK ? (int)len : CHUNK;
        int got;
        for (;;)
        {
            got = console_read(p->console, buf, want);
            if (got >= 0)
                break;
            /* Nothing typed yet. The thread waits without being scheduled; Enter in the window wakes it. Interrupts
             * are off, so the key cannot arrive between console_read and task_block and be missed. */
            console_wait(p->console, task_current());
            task_block();
        }
        if (got > 0)
            copy_to_user(ptr, buf, (unsigned)got);
        return got; /* 0 = the window is gone or the program is finished: end of input */
    }

    ull idx = fd - PROCESS_FD_FIRST;
    if (fd < PROCESS_FD_FIRST || idx >= PROCESS_MAX_FILES || p->file[idx].node < 0)
        return -1;
    const VfsNode *vn = vfs_node(p->file[idx].node);
    if (!vn)
        return -1; /* the file was deleted while open */
    ull total = 0;
    while (total < len)
    {
        if (p->file[idx].pos >= vn->size)
            break; /* at or past the end: 0 bytes, whatever a drive's read would say about such an offset */
        ull left = vn->size - p->file[idx].pos;
        unsigned n = (len - total) < CHUNK ? (unsigned)(len - total) : CHUNK;
        if (n > left)
            n = (unsigned)left;
        int got = vfs_read(p->file[idx].node, p->file[idx].pos, buf, n);
        if (got < 0)
            return total ? (long long)total : -1;
        if (got == 0)
            break; /* end of the file */
        copy_to_user(ptr + total, buf, (unsigned)got);
        p->file[idx].pos += (unsigned)got;
        total += (ull)got;
        if ((unsigned)got < n)
            break;
    }
    return (long long)total;
}

/* Paths: a name without a slash ("HELLO.ELF") is looked up in the top folder of every drive, like exec does; a path that
 * starts with '/' is a VFS path ("/RAM Disk/Documents/readme.txt"). Files are opened for reading only. */
static long long sys_open(const SyscallFrame *f)
{
    ull ptr = f->rdi, flags = f->rsi;
    if (flags != 0) /* O_RDONLY is all there is */
        return -1;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), ptr, as) < 0)
        return -1;
    int node = path[0] == '/' ? vfs_lookup(path) : exec_find(path);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || n->is_dir)
        return -1;
    for (int i = 0; i < PROCESS_MAX_FILES; i++)
        if (p->file[i].node < 0)
        {
            p->file[i].node = node;
            p->file[i].pos = 0;
            return PROCESS_FD_FIRST + i;
        }
    return -1; /* too many open files */
}

static long long sys_close(const SyscallFrame *f)
{
    ull fd = f->rdi;
    Process *p = process_current();
    if (!p)
        return -1;
    if (fd < PROCESS_FD_FIRST) /* closing the console is allowed and does nothing */
        return 0;
    ull idx = fd - PROCESS_FD_FIRST;
    if (idx >= PROCESS_MAX_FILES || p->file[idx].node < 0)
        return -1;
    p->file[idx].node = -1;
    return 0;
}

/* whence: 0 = from the start, 1 = from the current position, 2 = from the end. Returns the new position. A position past
 * the end is allowed (a read there returns 0); a negative one is not. */
static long long sys_seek(const SyscallFrame *f)
{
    ull fd = f->rdi;
    long long off = (long long)f->rsi;
    ull whence = f->rdx;
    Process *p = process_current();
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= PROCESS_MAX_FILES || p->file[idx].node < 0)
        return -1;
    const VfsNode *n = vfs_node(p->file[idx].node);
    if (!n)
        return -1;
    long long base;
    if (whence == 0)
        base = 0;
    else if (whence == 1)
        base = (long long)p->file[idx].pos;
    else if (whence == 2)
        base = (long long)n->size;
    else
        return -1;
    /* off is any 64-bit number from ring 3: base is between 0 and 2^32, so only a huge positive offset can overflow */
    if (off > 0 && off > 0x7FFFFFFFFFFFFFFFLL - base)
        return -1;
    long long pos = base + off;
    if (pos < 0 || pos > 0xFFFFFFFFLL)
        return -1;
    p->file[idx].pos = (unsigned)pos;
    return pos;
}

long long syscall_dispatch(SyscallFrame *f)
{
    COUNT++;
    switch (f->rax)
    {
    case SYS_WRITE:
        return sys_write(f);
    case SYS_EXIT:
        process_exit((int)f->rdi); /* does not return */
    case SYS_YIELD:
        task_yield();
        return 0;
    case SYS_READ:
        return sys_read(f);
    case SYS_OPEN:
        return sys_open(f);
    case SYS_CLOSE:
        return sys_close(f);
    case SYS_SEEK:
        return sys_seek(f);
    default:
        return -1;
    }
}


#if defined(__x86_64__) && !defined(HARIS_HOST)

typedef unsigned long long u64;

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE 1ULL

/* RFLAGS bits cleared on entry: IF (no interrupts until we are back), TF, DF (the C code expects it clear), NT, AC. */
#define SYSCALL_FLAG_MASK 0x44700ULL

/* One CPU, interrupts off from SYSCALL to SYSRET: plain variables do instead of per-CPU data. */
u64 syscall_user_rsp;
u64 syscall_kstack;

/* The code the CPU jumps to on SYSCALL. At this point RSP is still the user's. Frame layout = SyscallFrame.
 *
 * Alignment: the stack top is 16-aligned and the frame is 10 qwords (80 bytes), so RSP is 16-aligned after the pushes;
 * 32 more bytes are the shadow space of the Microsoft x64 call convention (arguments in rcx).
 *
 * Before SYSRET the user RIP in rcx must be a canonical address: a non-canonical one makes SYSRET fault in ring 0
 * while RSP is already the user's. The user RIP is the address after the SYSCALL instruction, which is canonical unless
 * the instruction sits at the very end of the lower half; such a process is ended instead. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl syscall_entry\n"
        "syscall_entry:\n"
        "    movq %rsp, syscall_user_rsp(%rip)\n"
        "    movq syscall_kstack(%rip), %rsp\n"
        "    pushq syscall_user_rsp(%rip)\n"
        "    pushq %r11\n"
        "    pushq %rcx\n"
        "    pushq %rax\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %rdx\n"
        "    pushq %r10\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    movq %rsp, %rcx\n"
        "    subq $128, %rsp\n" /* 32 shadow space + 96 for xmm0-5: the C code may change them, the program expects them back */
        "    movups %xmm0, 32(%rsp)\n"
        "    movups %xmm1, 48(%rsp)\n"
        "    movups %xmm2, 64(%rsp)\n"
        "    movups %xmm3, 80(%rsp)\n"
        "    movups %xmm4, 96(%rsp)\n"
        "    movups %xmm5, 112(%rsp)\n"
        "    cld\n"
        "    call syscall_dispatch\n"
        "    movups 32(%rsp), %xmm0\n"
        "    movups 48(%rsp), %xmm1\n"
        "    movups 64(%rsp), %xmm2\n"
        "    movups 80(%rsp), %xmm3\n"
        "    movups 96(%rsp), %xmm4\n"
        "    movups 112(%rsp), %xmm5\n"
        "    addq $128, %rsp\n"
        "    movq %rax, 48(%rsp)\n" /* the result goes into the saved rax */
        "    movq 56(%rsp), %rcx\n"
        "    movq %rcx, %rdx\n"
        "    sarq $47, %rdx\n"
        "    jz 1f\n"
        "    cmpq $-1, %rdx\n"
        "    jne 2f\n"
        "1:  popq %r9\n"
        "    popq %r8\n"
        "    popq %r10\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    popq %r11\n"
        "    popq %rsp\n" /* the user's stack again */
        "    sysretq\n"
        "2:  movq $-1, %rcx\n"
        "    subq $32, %rsp\n"
        "    call process_exit\n"
        "3:  hlt\n"
        "    jmp 3b\n");

void syscall_entry(void);

static u64 rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(unsigned msr, u64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}

void syscall_init(void)
{
    /* SYSCALL loads CS from STAR[47:32] (SS = +8: our kernel code and data). SYSRET to 64-bit code loads SS from
     * STAR[63:48] + 8 and CS from + 16, and it loads those numbers as they are: the real CPU does NOT turn the RPL
     * bits into 3 by itself (an emulator may, which hid this once). User code would then run with CS = 0x20 / SS = 0x18,
     * RPL 0, and the first interrupt would push those; IRETQ refuses a frame whose SS and CS have different RPL
     * (#GP). So the base carries the RPL: 0x13, giving SS = 0x1B and CS = 0x23 (SEL_SYSRET_BASE in segments.h). */
    u64 star = ((u64)SEL_SYSRET_BASE << 48) | ((u64)SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, SYSCALL_FLAG_MASK);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_set_kernel_stack(u64 top)
{
    syscall_kstack = top;
    gdt_set_kernel_stack(top); /* the same stack for interrupts that arrive while ring 3 code runs */
}

#else

/* Host tests: no instruction, no registers; the stack is just remembered. */
static unsigned long long KSTACK;
void syscall_init(void) {}
void syscall_set_kernel_stack(unsigned long long top)
{
    KSTACK = top;
    gdt_set_kernel_stack(top);
}

#endif
