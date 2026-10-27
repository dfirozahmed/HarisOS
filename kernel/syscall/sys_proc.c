/* System calls: processes (spawn, wait, fork, exec, dup2, signals, process list, heap). */
#include "syscall_internal.h"

/* The arguments of SYS_SPAWN / SYS_EXEC, copied into one blob "arg0\0arg1\0..." in a kernel heap block that grows as the strings come
 * (the kernel stack is too small for PROCESS_ARGS_BYTES). argv_ptr is a NULL-terminated array of string pointers, or 0 = just the path
 * as argv[0]. 0 and *blob_out / *len_out / *argc_out, or -1 (bad pointers, too many or too long, no memory). *blob_out is set even when
 * it fails (NULL or a block): the caller always kfrees it. */
typedef struct
{
    char *p;
    unsigned len, cap;
} ArgBlob;

/* Append the NUL-terminated string at user address `src` (the pages are checked one at a time as the string runs into them). 0, or -1. */
static int blob_append_string(ArgBlob *b, const AddressSpace *as, ull src)
{
    for (ull i = 0;; i++)
    {
        ull a = src + i;
        if ((i == 0 || (a & 4095) == 0) && !aspace_user_ok(as, a, 1, 0))
            return -1;
        if (b->len + 1 > PROCESS_ARGS_BYTES)
            return -1; /* the arguments together do not fit */
        if (b->len + 1 > b->cap)
        {
            unsigned cap = b->cap ? b->cap * 2 : 256;
            char *n = krealloc(b->p, cap);
            if (!n)
                return -1;
            b->p = n;
            b->cap = cap;
        }
        char c;
        copy_from_user(&c, a, 1);
        b->p[b->len++] = c;
        if (c == 0)
            return 0;
    }
}

static int collect_args(const AddressSpace *as, ull path_ptr, ull argv_ptr, char **blob_out, unsigned *len_out, int *argc_out)
{
    ArgBlob b = {0, 0, 0};
    int argc = 0, ok = 1;
    if (argv_ptr == 0)
    {
        ok = blob_append_string(&b, as, path_ptr) == 0;
        argc = 1;
    }
    else
    {
        for (;; argc++)
        {
            if (argc == PROCESS_ARGS_MAX)
            {
                ok = 0; /* no terminating NULL within the limit */
                break;
            }
            ull slot_va = argv_ptr + (ull)argc * 8;
            ull str;
            if (slot_va < argv_ptr || !aspace_user_ok(as, slot_va, 8, 0))
            {
                ok = 0;
                break;
            }
            copy_from_user((char *)&str, slot_va, 8);
            if (str == 0)
                break;
            if (blob_append_string(&b, as, str) != 0)
            {
                ok = 0; /* unreadable, or the arguments together do not fit */
                break;
            }
        }
    }
    *blob_out = b.p;
    *len_out = b.len;
    *argc_out = argc;
    return ok ? 0 : -1;
}

/* SYS_SPAWN: path in rdi (as for SYS_OPEN), argv in rsi (a NULL-terminated array of string pointers, or 0 = just the path
 * as argv[0]). Starts the program as a child that shares this process's console. Returns the child's pid, or -1: no such
 * program, not a runnable file, bad pointers, too many or too long arguments, too many children not waited for yet, or no
 * memory / thread slot. Reading the program file happens inside this call. */
static long long spawn_with_blob(const SyscallFrame *f, char **blob_out)
{
    ull path_ptr = f->rdi, argv_ptr = f->rsi, io_ptr = f->rdx;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), path_ptr, as) < 0)
        return -1;
    int slot = process_child_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1; /* wait for some of them first */
    int node = resolve_path(p, path, 1);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || n->is_dir)
        return -1;

    char *blob;
    unsigned len = 0;
    int argc = 0;
    int collected = collect_args(as, path_ptr, argv_ptr, &blob, &len, &argc);
    *blob_out = blob; /* (the caller frees it, whatever happens below) */
    if (collected != 0)
        return -1;

    /* fd 0, 1, 2 of the child: the files the caller named (its own open descriptors, copied), else what the caller itself has
     * (a redirected shell passes it on), else the console */
    int io[3] = {-1, -1, -1};
    if (io_ptr)
    {
        if (!aspace_user_ok(as, io_ptr, sizeof(io), 0))
            return -1;
        copy_from_user((char *)io, io_ptr, sizeof(io));
    }
    ProcessFile redir[3];
    int any_redirect = 0;
    for (int k = 0; k < 3; k++)
    {
        redir[k].node = -1;
        redir[k].pos = redir[k].flags = 0;
        int idx = p->stdio[k];
        if (io[k] != -1)
        {
            if (io[k] < PROCESS_FD_FIRST || io[k] >= PROCESS_FD_FIRST + p->file_cap)
                return -1;
            idx = io[k] - PROCESS_FD_FIRST;
            if (p->file[idx].node < 0 || pfile_is_object(&p->file[idx]) || ((p->file[idx].flags & PFILE_WRITE) != 0) != (k != 0))
                return -1; /* not open, or opened the wrong way round (input must be readable, output writable) */
        }
        if (idx >= 0)
        {
            redir[k] = p->file[idx];
            any_redirect = 1;
        }
    }

    ProcessSpawn sp = {.console = p->console, .argc = argc, .args = blob, .args_len = len, .parent = task_current(),
                       .cwd = p->cwd, .redirect = any_redirect ? redir : 0};
    int err = 0;
    int id = exec_spawn_node(node, n->name, &sp, &err);
    if (id < 0)
        return -1;
    p->child[slot] = sp.pid;
    return sp.pid;
}

/* The child's stack has its own copy of the arguments by the time the call returns: the blob is not needed any more. */
long long sys_spawn(const SyscallFrame *f)
{
    char *blob = 0;
    long long r = spawn_with_blob(f, &blob);
    kfree(blob);
    return r;
}

/* SYS_WAIT: pid in rdi (from SYS_SPAWN), a pointer for the exit code in rsi (or 0). Waits, without using the CPU, until
 * that child has ended; then returns the pid and stores the exit code (-1 if a CPU exception ended it). Each child can be
 * waited for once. -1 if pid is not a child of this process that was not waited for, or the pointer is bad (the child is
 * then still there to wait for). */
long long sys_wait(const SyscallFrame *f)
{
    ull want = f->rdi, status_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || want == 0 || want > 0x7FFFFFFF)
        return -1;
    int slot = -1;
    for (int i = 0; i < p->child_cap; i++)
        if (p->child[i] == (int)want)
            slot = i;
    if (slot < 0)
        return -1;
    if (status_ptr && !aspace_user_ok(as, status_ptr, 4, 1))
        return -1;
    int code;
    while (!process_reap((int)want, &code))
    {
        task_block(); /* the child's exit wakes this thread; interrupts are off, so that cannot slip in between */
        if (sig_interrupted(p))
            return -1; /* the child stays waitable */
    }
    p->child[slot] = 0;
    if (status_ptr)
        copy_to_user(status_ptr, (const char *)&code, 4);
    return (long long)want;
}

/* ---- fork, exec, dup2, getpid ----------------------------------------------------------------------------------------------- */
/* SYS_FORK: a copy of this process (see process_fork). The parent gets the child's pid, the child starts after the SYSCALL
 * instruction with rax = 0 and the registers of the parent NOT preserved (the fork() of the user library saves them on the stack
 * first). -1: too many children, no memory. */
long long sys_fork(const SyscallFrame *f)
{
    if (!process_current())
        return -1;
    return process_fork(f->rcx, f->user_rsp);
}

/* SYS_EXEC: rdi = path (as for SYS_SPAWN), rsi = argv. This process becomes that program: same pid, console, folder, open files
 * and redirections. Does not return when it works. -1 (and nothing changed): no such program, not an ELF file, bad pointers, no
 * memory. */
static long long exec_with_blob(const SyscallFrame *f, char **blob_out)
{
    ull path_ptr = f->rdi, argv_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), path_ptr, as) < 0)
        return -1;
    int node = resolve_path(p, path, 1);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || n->is_dir)
        return -1;
    char *blob;
    unsigned len = 0;
    int argc = 0;
    int collected = collect_args(as, path_ptr, argv_ptr, &blob, &len, &argc);
    *blob_out = blob; /* (freed by sys_exec if we come back) */
    if (collected != 0)
        return -1;
    ProcessSpawn sp = {.argc = argc, .args = blob, .args_len = len, .free_args = 1}; /* a program that starts frees it itself, we never return then */
    int err = 0;
    exec_replace_node(node, n->name, &sp, &err); /* returns only if it failed */
    return -1;
}

long long sys_exec(const SyscallFrame *f)
{
    char *blob = 0;
    long long r = exec_with_blob(f, &blob); /* (it does not come back when the new program starts: the blob was freed on the way) */
    kfree(blob);
    return r;
}

/* SYS_GETPID: rdi = 0 for this process's pid, 1 for the parent's (0 if it has none or is gone). */
long long sys_getpid(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p || f->rdi > 1)
        return -1;
    return f->rdi == 0 ? p->pid : process_ppid();
}

/* Let go of the descriptor that stands in for fd k (0..2): its file is closed unless fd 1 and 2 still share it. */
static void release_stdio(Process *p, int k)
{
    int slot = p->stdio[k];
    p->stdio[k] = -1;
    if (slot < 0)
        return;
    for (int j = 0; j < 3; j++)
        if (p->stdio[j] == slot)
            return;
    pfile_close(&p->file[slot]);
}

/* SYS_DUP2: rdi = old descriptor, rsi = new = 0, 1 or 2. Afterwards fd `new` is the same open file as `old` (a copy in a slot of its
 * own: closing `old` later does not close `new`, and a pipe end is counted for both). old = a file descriptor from SYS_OPEN or
 * SYS_PIPE, or 0..2 (copy what that one is now: "2>&1"), or -1 = the console. Returns `new`, or -1 (no free slot, bad fd, the
 * wrong direction: fd 0 must be readable, fd 1 and 2 writable). A child keeps these across SYS_EXEC: this is how a fork-and-exec
 * shell makes redirections. */
long long sys_dup2(const SyscallFrame *f)
{
    Process *p = process_current();
    long long old = (long long)f->rdi;
    ull nw = f->rsi;
    if (!p || nw > 2)
        return -1;
    int src = -1; /* the file slot to copy, -1 = the console */
    if (old == -1)
        src = -1;
    else if (old >= 0 && old <= 2)
        src = p->stdio[old];
    else if (old >= PROCESS_FD_FIRST && old < PROCESS_FD_FIRST + p->file_cap && p->file[old - PROCESS_FD_FIRST].node >= 0)
        src = (int)old - PROCESS_FD_FIRST;
    else
        return -1;
    if (src >= 0 && (pfile_is_object(&p->file[src]) || ((p->file[src].flags & PFILE_WRITE) != 0) != (nw != 0)))
        return -1; /* input must be readable, output writable; a message queue is neither */
    if (src >= 0 && p->stdio[nw] == src)
        return (long long)nw; /* already so */
    int slot = -1;
    if (src >= 0)
    {
        slot = process_file_slot(p); /* (may move the list: src is an index) */
        if (slot < 0)
            return -1;
    }
    release_stdio(p, (int)nw);
    if (src >= 0)
    {
        p->file[slot] = p->file[src];
        pfile_ref(&p->file[slot]);
        p->stdio[nw] = slot;
    }
    return (long long)nw;
}

/* ---- signal handlers --------------------------------------------------------------------------------------------------------- */
long long sys_sigaction(const SyscallFrame *f)
{
    Process *p = process_current();
    ull sig = f->rdi, handler = f->rsi, entry = f->rdx;
    if (!p || (sig != SIGINT && sig != SIGTERM))
        return -1;
    if (handler == 0)
    {
        p->sig_handler[sig] = 0;
        p->sig_pending &= ~(1u << sig);
        return 0;
    }
    if (handler < PAGING_USER_BASE || handler >= PAGING_USER_END || entry < PAGING_USER_BASE || entry >= PAGING_USER_END)
        return -1;
    p->sig_handler[sig] = handler;
    p->sig_entry = entry;
    p->sig_ignore &= ~(1u << sig); /* installing a handler ends "ignore" */
    return 0;
}

long long sys_sigmask(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p)
        return -1;
    unsigned old = p->sig_blocked;
    p->sig_blocked = (unsigned)f->rdi & SIG_HANDLER_MASK; /* a signal that was waiting runs as this call returns */
    return old;
}

/* ---- signals and process list ---------------------------------------------------------------------------------------- */
long long sys_kill(const SyscallFrame *f)
{
    if (!process_current())
        return -1;
    ull pid = f->rdi, sig = f->rsi;
    if (pid == 0 || pid > 0x7FFFFFFF || (sig != 0 && sig != SIGINT && sig != SIGKILL && sig != SIGTERM))
        return -1;
    return process_signal((int)pid, (int)sig) == 0 ? 0 : -1; /* (a process that signals itself does not come back) */
}

/* SYS_SETPRIO: rdi = pid (0 = the caller), rsi = new priority (0 .. 4, higher runs first) or -1 to only read it. Only the caller itself and
 * its children (not yet waited for) can be changed. Returns the priority the process had, or -1. */
long long sys_setprio(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p || f->rdi > 0x7FFFFFFF || (f->rsi != ~0ULL && f->rsi > (ull)TASK_PRIO_MAX))
        return -1;
    int pid = f->rdi == 0 ? p->pid : (int)f->rdi;
    int allowed = pid == p->pid;
    for (int i = 0; i < p->child_cap && !allowed; i++)
        allowed = p->child[i] == pid;
    if (!allowed)
        return -1;
    return process_priority(pid, f->rsi == ~0ULL ? -1 : (int)f->rsi);
}

long long sys_sigignore(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p)
        return -1;
    p->sig_ignore = (unsigned)f->rdi & ((1u << SIGINT) | (1u << SIGTERM)); /* SIGKILL cannot be ignored; no other exists */
    return 0;
}

long long sys_psinfo(const SyscallFrame *f)
{
    ull index = f->rdi, out_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    if (!as || !process_current() || !aspace_user_ok(as, out_ptr, sizeof(SysPsInfo), 1))
        return -1;
    if (index >= (ull)task_slots())
        return 0;
    ProcessInfo pi;
    if (!process_info((int)index, &pi))
        return 0;
    SysPsInfo e;
    e.pid = pi.pid;
    e.ppid = pi.ppid;
    e.running = pi.running;
    e.console = pi.console;
    for (int i = 0; i < (int)sizeof(e.name); i++)
        e.name[i] = i < (int)sizeof(pi.name) ? pi.name[i] : 0;
    copy_to_user(out_ptr, (const char *)&e, sizeof(e));
    return 1;
}

long long sys_brk(const SyscallFrame *f)
{
    ull want = f->rdi;
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    if (want == 0)
        return (long long)p->heap_brk;
    if (want < p->heap_base || want > p->heap_limit)
        return -1;
    ull need = (want + 4095) & ~4095ULL; /* want <= heap_limit < 2^47: no overflow */
    while (p->heap_mapped < need)
    {
        if (aspace_map_new(as, p->heap_mapped, ASPACE_WRITE) != 0)
            return -1;
        p->heap_mapped += 4096;
    }
    p->heap_brk = want;
    return (long long)want;
}

