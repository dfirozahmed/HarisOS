/* Processes: entering ring 3, the stack and arguments, launching, starting from an ELF file. */
#include "process_internal.h"

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
        BKL_ASM_UNLOCK /* the processor leaves the kernel (bkl.h) */
        "    swapgs\n" /* ring 3 gets its GS back, the kernel's waits in IA32_KERNEL_GS_BASE (percpu.h); interrupts are off here */
        "    iretq\n");

_Static_assert(SEL_USER_DATA == 0x1B && SEL_USER_CODE == 0x23, "process_enter_user pushes these selectors as numbers");

#else

__attribute__((noreturn)) void process_enter_user(u64 entry, u64 user_rsp, u64 argc, u64 argv)
{
    (void)entry;
    (void)user_rsp;
    (void)argc;
    (void)argv;
    for (;;)
        ;
}

#endif


/* The thread's first code. The scheduler has already loaded the process's address space and set the kernel stack that
 * ring 3 traps will land on; all that is left is to go to ring 3. */
void user_thread_main(void *arg)
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
void process_free(void *p)
{
    process_release_files((Process *)p); /* (normally done at exit: this catches any other way a process can go away) */
    kfree(p);
}

int NEXT_PID = 1;

/* The arguments go to the top of the stack, then the argv array below them, then the (fake) return address:
 *
 *   PAGING_USER_END   -> +--------------------+
 *                        | "arg0\0arg1\0..."  |   the strings, as given
 *                        | argv[0..argc-1]    |   pointers to them, then NULL (16-aligned start)
 *   user_rsp          -> | 0 (return address) |   so rsp is 8 mod 16 at the first instruction
 *
 * The blob is checked here as well (exactly argc strings, all inside args_len): it is built by the kernel, but a mistake
 * must not become a write outside the stack. Returns 0, or -1 (nothing is written then). */
/* How many pages the stack starts with: PROCESS_STACK_PAGES, or more when the arguments (the strings, the argv array, the alignment and the
 * return slot, and a page of room for the program) do not fit. */
unsigned stack_pages_for(const ProcessSpawn *sp)
{
    unsigned long long need = (unsigned long long)sp->args_len + (unsigned long long)(sp->argc + 1) * 8 + 64;
    unsigned pages = (unsigned)((need + 4095) / 4096) + 1;
    return pages > PROCESS_STACK_PAGES ? pages : PROCESS_STACK_PAGES;
}

int place_args(AddressSpace *as, const ProcessSpawn *sp, Start *st)
{
    int argc = sp->argc;
    unsigned len = sp->args_len;
    if (argc < 0 || argc > PROCESS_ARGS_MAX || len > PROCESS_ARGS_BYTES || (argc > 0 && (!sp->args || len == 0)))
        return -1;
    u64 *argv_ptr = kmalloc((u64)(argc + 1) * 8); /* (the kernel stack is small: the array lives on the heap) */
    if (!argv_ptr)
        return -1;
    unsigned off = 0;
    u64 str_base = PAGING_USER_END - len;
    for (int i = 0; i < argc; i++)
    {
        if (off >= len)
            goto bad;
        argv_ptr[i] = str_base + off;
        while (off < len && sp->args[off])
            off++;
        if (off >= len)
            goto bad; /* a string without its NUL */
        off++;
    }
    if (off != len)
        goto bad; /* bytes left over */
    argv_ptr[argc] = 0;
    u64 array_bytes = (u64)(argc + 1) * 8;
    u64 array_base = (str_base - array_bytes) & ~15ULL;
    if (len && copy_in(as, str_base, (const unsigned char *)sp->args, len) != 0)
        goto bad;
    if (copy_in(as, array_base, (const unsigned char *)argv_ptr, array_bytes) != 0)
        goto bad;
    kfree(argv_ptr);
    st->argc = (u64)argc;
    st->argv = array_base;
    st->user_rsp = array_base - 8;
    return 0;
bad:
    kfree(argv_ptr);
    return -1;
}

/* The heap of a program whose last segment ends at heap_base. It may not run into the stack: it stops below the 1 MiB the stack
 * may grow into (ELF_STACK_RESERVE). */
void set_heap(Process *p, u64 heap_base)
{
    u64 stack_low = PAGING_USER_END - ELF_STACK_RESERVE;
    p->heap_base = p->heap_brk = p->heap_mapped = heap_base;
    p->heap_limit = heap_base + PROCESS_HEAP_MAX < stack_low ? heap_base + PROCESS_HEAP_MAX : stack_low;
    if (p->heap_limit < heap_base)
        p->heap_limit = heap_base; /* the program sits that high: no heap */
}

void purge_exits(int owner); /* (the records of exit codes, further down) */

static int launch(const char *name, AddressSpace *as, u64 entry, ProcessSpawn *sp, u64 heap_base)
{
    unsigned stack_pages = stack_pages_for(sp);
    for (unsigned i = 1; i <= stack_pages; i++)
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
    p->sig_ignore = 0;
    for (int i = 0; i < 32; i++)
        p->sig_handler[i] = 0;
    p->sig_entry = 0;
    p->sig_pending = p->sig_blocked = 0;
    p->shm = 0; /* (the list of shared memory mappings is made by the first SYS_SHM_MAP) */
    p->shm_cap = 0;
    p->shm_next = 0;
    p->child = 0; /* (the list of children is made by the first spawn or fork) */
    p->child_cap = 0;
    set_heap(p, heap_base);
    p->stack_low = PAGING_USER_END - (u64)stack_pages * 4096;
    if (process_files_init(p) != 0)
    {
        kfree(st);
        kfree(p);
        aspace_destroy(as);
        return -1;
    }
    for (int k = 0; k < 3; k++)
        p->stdio[k] = -1;
    if (sp->redirect) /* shell redirection: files stand in for fd 0, 1, 2 (only 3 of the PROCESS_FILES_FIRST entries) */
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
        kfree(p->file);
        kfree(p);
        aspace_destroy(as);
        return -1;
    }
    for (int i = 0; i < p->file_cap; i++) /* the child holds the pipe ends it was given */
        pfile_ref(&p->file[i]);
    STATS.started++;
    sp->pid = NEXT_PID++;
    return id;
}

Process *process_current(void) { return (Process *)task_user_data(); }

void process_release_files(Process *p)
{
    if (!p)
        return;
    process_release_shm(p);
    gui_release_owner(p->pid); /* its windows go away with it */
    for (int i = 0; i < p->file_cap; i++)
        pfile_close(&p->file[i]);
    kfree(p->file); /* the list itself (NULL is fine) */
    p->file = 0;
    p->file_cap = 0;
    purge_exits(p->pid); /* nobody can collect the exit codes of its children now */
    kfree(p->child);

    p->child = 0;
    p->child_cap = 0;
    for (int k = 0; k < 3; k++)
        p->stdio[k] = -1;
}

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

/* Each segment gets its own pages (elf_parse made sure no page is shared) with the permissions it asks for. A fresh page is zero,
 * which is exactly what the bytes after filesz (.bss) must be; only the file's bytes are copied. *heap_base = the first page after
 * the last segment. 0, or -1 (out of memory: the caller destroys `as`). */
int load_segments(AddressSpace *as, const ElfInfo *info, const void *file, u64 *heap_base)
{
    *heap_base = 0;
    for (int i = 0; i < info->nseg; i++)
    {
        const ElfSegment *s = &info->seg[i];
        u64 seg_end = (s->vaddr + s->memsz + 4095) & ~4095ULL;
        if (seg_end > *heap_base)
            *heap_base = seg_end;
        unsigned flags = ((s->flags & ELF_PF_W) ? ASPACE_WRITE : 0) | ((s->flags & ELF_PF_X) ? ASPACE_EXEC : 0);
        u64 first = s->vaddr & ~4095ULL, last = (s->vaddr + s->memsz - 1) & ~4095ULL;
        for (u64 va = first; va <= last; va += 4096)
            if (aspace_map_new(as, va, flags) != 0)
                return -1;
        if (copy_in(as, s->vaddr, (const unsigned char *)file + s->offset, s->filesz) != 0)
            return -1;
    }
    return 0;
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

    u64 heap_base;
    if (load_segments(as, &info, file, &heap_base) != 0)
    {
        aspace_destroy(as);
        return -1;
    }
    return launch(name, as, info.entry, sp, heap_base);
}
