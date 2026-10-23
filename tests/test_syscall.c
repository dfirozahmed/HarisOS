/* Host test for the system call dispatcher (kernel/core/syscall.c): what each call does with its arguments. The entry
 * code (SYSCALL instruction, register saving) is hardware and is only built for the kernel; the checks a call makes on
 * pointers from ring 3, the file descriptors and the console are what matters most, and they are tested here against
 * small fakes of the console, the scheduler and the file system. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
/* The MinGW <stdio.h> defines a macro SYS_OPEN of its own (for _sopen); the kernel's call number is the one meant here. */
#undef SYS_OPEN
#include "syscall.h"
#include "aspace.h"
#include "console.h"
#include "exec.h"
#include "gui.h"
#include "keyboard.h"
#include "pmm.h"
#include "process.h"
#include "task.h"
#include "vfs.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef unsigned long long ull;

/* ---- the address space and the user memory check ---- */
static AddressSpace FAKE_AS;
static AddressSpace *CURRENT_AS = &FAKE_AS;
static int USER_OK = 1, USER_OK_CALLS;
static ull OK_VA, OK_LEN;
static int OK_WRITE = -1;
static ull BLOCK_FROM; /* user_ok fails for any range that reaches this address (0 = off): an unmapped page */

struct AddressSpace *task_address_space(void) { return CURRENT_AS; }

int aspace_user_ok(const AddressSpace *as, ull va, ull len, int write)
{
    CHECK(as == &FAKE_AS);
    USER_OK_CALLS++;
    OK_VA = va;
    OK_LEN = len;
    OK_WRITE = write;
    if (BLOCK_FROM && va + len > BLOCK_FROM)
        return 0;
    return USER_OK;
}

/* the heap: pages are only counted; MAP_FAIL_AFTER > 0 makes the Nth mapping from now on fail */
static int MAPPED_PAGES, MAP_FAIL_AFTER;
static ull LAST_MAPPED_VA;

int aspace_map_new(AddressSpace *as, ull va, unsigned flags)
{
    CHECK(as == &FAKE_AS && (va & 4095) == 0 && flags == ASPACE_WRITE); /* writable, never executable */
    if (MAP_FAIL_AFTER > 0 && --MAP_FAIL_AFTER == 0)
        return -1;
    MAPPED_PAGES++;
    LAST_MAPPED_VA = va;
    return 0;
}

/* ---- the user-memory window (SMAP): it must be open exactly while user memory is touched ---- */
volatile int host_uaccess;

/* ---- the process record and the scheduler ---- */
static Process PROC;
static Process *CURRENT_PROC = &PROC;
static int BLOCKS, YIELDS, EXIT_CODE = -12345;
static jmp_buf EXIT_JMP;

Process *process_current(void) { return CURRENT_PROC; }
int task_current(void) { return 7; }
void task_yield(void) { YIELDS++; }
static int WOKEN_TASKS[16], WOKEN_TASKS_N;
void task_wake(int id) { if (WOKEN_TASKS_N < 16) WOKEN_TASKS[WOKEN_TASKS_N++] = id; } /* (pipes wake waiters) */
/* mutex.c: every thread sleeps (so a wake-up is worth sending) except DEAD_TASK, which has ended; interrupts are a flag it can look at */
volatile int host_if = 1;
static int DEAD_TASK = -1;
TaskState task_state(int id) { return id == DEAD_TASK ? TASK_DEAD : TASK_BLOCKED; }
int task_effective_priority(int id) { (void)id; return TASK_PRIO_DEFAULT; }
int task_set_inherited(int id, int prio) { (void)id; (void)prio; return 0; }

void process_exit(int code)
{
    EXIT_CODE = code;
    longjmp(EXIT_JMP, 1); /* the real one never returns; neither does this: it jumps out of the dispatcher */
}

void gdt_set_kernel_stack(ull rsp0) { (void)rsp0; }

/* ---- serial and console ---- */
static char SERIAL[4096];
static int SERIAL_N;

void serial_putc(char c)
{
    CHECK(host_uaccess == 0); /* code that talks to devices never runs with the user-memory window open */
    if (SERIAL_N < (int)sizeof(SERIAL) - 1)
        SERIAL[SERIAL_N++] = c;
    SERIAL[SERIAL_N] = 0;
}

static char CON_OUT[8192];
static int CON_OUT_N, CON_WRITES, CON_VALID = 1, CON_FINISHED, CON_BLOCKS_BEFORE_DATA, CON_WAITS;
static const char *CON_LINE = "hello\n"; /* what the "user" typed */
static int CON_LINE_POS;
static int CON_WAIT_TASK = -1;

int console_valid(int con) { return con >= 0 && CON_VALID; }

int console_write(int con, const char *text, unsigned n)
{
    CHECK(host_uaccess == 0);
    if (!console_valid(con))
        return -1;
    CON_WRITES++;
    for (unsigned i = 0; i < n && CON_OUT_N < (int)sizeof(CON_OUT) - 1; i++)
        CON_OUT[CON_OUT_N++] = text[i];
    CON_OUT[CON_OUT_N] = 0;
    return (int)n;
}

int console_read(int con, void *buf, int len)
{
    if (!console_valid(con))
        return 0;
    if (CON_BLOCKS_BEFORE_DATA > 0)
        return -1; /* nothing typed yet */
    int left = (int)strlen(CON_LINE) - CON_LINE_POS;
    if (left == 0)
        return CON_FINISHED ? 0 : -1;
    int n = left < len ? left : len;
    memcpy(buf, CON_LINE + CON_LINE_POS, (size_t)n);
    CON_LINE_POS += n;
    return n;
}

void console_wait(int con, int task_id)
{
    (void)con;
    CON_WAIT_TASK = task_id;
    CON_WAITS++;
}

static int PENDING_PID, PENDING_CODE, DONE_PID, DONE_CODE; /* a child that "ends" while the parent sleeps in task_block */
static void (*BLOCK_ACTION)(void); /* what happens "elsewhere" while the caller sleeps in task_block (a pipe peer runs) */

void task_block(void)
{
    CHECK(host_uaccess == 0);
    BLOCKS++;
    if (CON_BLOCKS_BEFORE_DATA > 0)
        CON_BLOCKS_BEFORE_DATA--; /* "the user pressed Enter while we slept" */
    if (BLOCK_ACTION)
        BLOCK_ACTION();
    if (PENDING_PID)
    {
        DONE_PID = PENDING_PID;
        DONE_CODE = PENDING_CODE;
        PENDING_PID = 0;
    }
}

/* ---- spawn and wait: the loader and the exit records are fakes ---- */
static int SPAWN_CALLS, SPAWN_FAIL, SPAWN_NEXT_PID = 100;
static int SP_NODE, SP_ARGC, SP_CONSOLE, SP_PARENT, SP_CWD;
static ProcessFile SP_REDIR[3];
static int SP_REDIR_N; /* 3 if the last spawn had a redirect array, else 0 */
static unsigned SP_LEN;
static char SP_ARGS[PROCESS_ARGS_BYTES], SP_NAME[64];


int exec_spawn_node(int node, const char *name, ProcessSpawn *sp, int *err)
{
    (void)err;
    CHECK(host_uaccess == 0);
    SPAWN_CALLS++;
    if (SPAWN_FAIL)
        return -5;
    SP_NODE = node;
    strcpy(SP_NAME, name);
    SP_ARGC = sp->argc;
    SP_LEN = sp->args_len;
    SP_CONSOLE = sp->console;
    SP_PARENT = sp->parent;
    SP_CWD = sp->cwd;
    SP_REDIR_N = sp->redirect ? 3 : 0;
    if (sp->redirect)
        memcpy(SP_REDIR, sp->redirect, sizeof(SP_REDIR));
    memcpy(SP_ARGS, sp->args, sp->args_len);
    sp->pid = SPAWN_NEXT_PID++;
    return 3;
}

int process_reap(int pid, int *code)
{
    if (pid != DONE_PID || DONE_PID == 0)
        return 0;
    *code = DONE_CODE;
    DONE_PID = 0;
    return 1;
}

/* ---- the file system: a small tree that can change ----
 *   0 /                 the root
 *   6   RAM Disk        the one volume
 *   2     DIR           an empty folder
 *   3     readme.txt    20 bytes (reads give a pattern)
 *   1 HELLO.ELF, 4 BAD.BIN, 5 EMPTY.BIN   not in the tree: only exec_find reaches them (like programs on the boot drive)
 * Nodes made by vfs_add are 7 and up; they keep their bytes (up to FDATA_MAX), so what a program wrote can be read back. */
#define NNODES 24
#define FDATA_MAX 4096
static VfsNode NODES[NNODES];
static unsigned char FDATA[NNODES][FDATA_MAX];
static int READ_CALLS;
static int VFS_READONLY;   /* 1 = every change is refused (a locked drive) */
static int WRITES, ADDS, REMOVES;

static unsigned char pattern(int id, unsigned off) { return (unsigned char)(off * 7 + 3 + (unsigned)id); }

static void setup_vfs(void)
{
    memset(NODES, 0, sizeof(NODES));
    strcpy(NODES[1].name, "HELLO.ELF"); NODES[1].used = 1; NODES[1].size = 600;  NODES[1].parent = -2;
    strcpy(NODES[2].name, "DIR");       NODES[2].used = 1; NODES[2].is_dir = 1;  NODES[2].parent = 6;
    strcpy(NODES[3].name, "readme.txt"); NODES[3].used = 1; NODES[3].size = 20;  NODES[3].parent = 6;
    strcpy(NODES[4].name, "BAD.BIN");   NODES[4].used = 1; NODES[4].size = 2000; NODES[4].parent = -2;
    strcpy(NODES[5].name, "EMPTY.BIN"); NODES[5].used = 1; NODES[5].size = 0;    NODES[5].parent = -2;
    NODES[0].used = 1; NODES[0].is_dir = 1; NODES[0].parent = -1;
    strcpy(NODES[6].name, "RAM Disk"); NODES[6].used = 1; NODES[6].is_dir = 1; NODES[6].parent = 0;
    VFS_READONLY = 0;
}

const VfsNode *vfs_node(int id) { return id >= 0 && id < NNODES && NODES[id].used ? &NODES[id] : 0; }

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32))
            return 0;
    return *a == *b;
}

int exec_find(const char *name)
{
    for (int i = 1; i <= 5; i++)
        if (!NODES[i].is_dir && ieq(NODES[i].name, name))
            return i;
    return -1;
}

static int child_named(int dir, const char *name)
{
    for (int i = 0; i < NNODES; i++)
        if (NODES[i].used && NODES[i].parent == dir && i != dir && ieq(NODES[i].name, name))
            return i;
    return -1;
}

int vfs_lookup(const char *path)
{
    if (path[0] != '/')
        return -1;
    int cur = 0;
    while (*path)
    {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        char part[96];
        int n = 0;
        while (*path && *path != '/')
        {
            if (n >= 95)
                return -1; /* a name longer than the VFS keeps */
            part[n++] = *path++;
        }
        part[n] = 0;
        if (!NODES[cur].is_dir)
            return -1;
        cur = child_named(cur, part);
        if (cur < 0)
            return -1;
    }
    return cur;
}

/* children: folders first, then files, each by name */
int vfs_list(int dir, int *out, int max)
{
    int all[NNODES], n = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        int first = n;
        for (int i = 0; i < NNODES; i++)
            if (NODES[i].used && NODES[i].parent == dir && i != dir && (NODES[i].is_dir != 0) == (pass == 0))
                all[n++] = i;
        for (int a = first + 1; a < n; a++) /* insertion sort by name */
            for (int b = a; b > first && strcmp(NODES[all[b]].name, NODES[all[b - 1]].name) < 0; b--)
            {
                int t = all[b];
                all[b] = all[b - 1];
                all[b - 1] = t;
            }
    }
    for (int i = 0; i < n && i < max; i++)
        out[i] = all[i];
    return n;
}

int vfs_path(int id, char *out, int cap)
{
    char tmp[256];
    int len = 0;
    int chain[16], depth = 0;
    for (int n = id; n > 0 && depth < 16; n = NODES[n].parent)
        chain[depth++] = n;
    if (depth == 0)
        strcpy(tmp, "/");
    else
    {
        tmp[0] = 0;
        for (int d = depth - 1; d >= 0; d--)
        {
            strcat(tmp, "/");
            strcat(tmp, NODES[chain[d]].name);
        }
    }
    len = (int)strlen(tmp);
    if (len + 1 > cap)
        len = cap - 1;
    memcpy(out, tmp, (size_t)len);
    out[len] = 0;
    return len;
}

int vfs_read(int id, unsigned offset, void *buf, unsigned len)
{
    CHECK(host_uaccess == 0);
    READ_CALLS++;
    const VfsNode *n = vfs_node(id);
    if (!n)
        return -1;
    unsigned char *p = (unsigned char *)buf;
    if (id >= 7) /* a file made here: its own bytes */
    {
        if (offset >= n->size)
            return 0;
        unsigned got = n->size - offset < len ? n->size - offset : len;
        memcpy(p, FDATA[id] + offset, got);
        return (int)got;
    }
    if (id == 4 && offset >= 1024)
        return -1; /* a drive error half way through the file */
    unsigned end = id == 4 ? 1024 : n->size;
    if (offset >= end)
        return 0;
    unsigned got = end - offset < len ? end - offset : len;
    for (unsigned i = 0; i < got; i++)
        p[i] = pattern(id, offset + i);
    return (int)got;
}

int vfs_add(int parent, const char *name, int is_dir, unsigned size)
{
    CHECK(host_uaccess == 0);
    const VfsNode *d = vfs_node(parent);
    if (VFS_READONLY || !d || !d->is_dir || !name[0] || strchr(name, '/') || child_named(parent, name) >= 0)
        return -1;
    for (int i = 7; i < NNODES; i++)
        if (!NODES[i].used)
        {
            memset(&NODES[i], 0, sizeof(NODES[i]));
            strcpy(NODES[i].name, name);
            NODES[i].used = 1;
            NODES[i].is_dir = is_dir;
            NODES[i].size = size;
            NODES[i].parent = parent;
            memset(FDATA[i], 0, FDATA_MAX);
            ADDS++;
            return i;
        }
    return -1;
}

int vfs_write(int id, unsigned offset, const void *buf, unsigned len)
{
    CHECK(host_uaccess == 0);
    const VfsNode *n = vfs_node(id);
    if (VFS_READONLY || !n || n->is_dir || id < 7 || offset > FDATA_MAX)
        return -1; /* (the ready-made files have no bytes, like the RAM Disk's) */
    unsigned put = FDATA_MAX - offset < len ? FDATA_MAX - offset : len;
    if (put == 0)
        return -1; /* the drive is full */
    memcpy(FDATA[id] + offset, buf, put);
    if (offset + put > NODES[id].size)
        NODES[id].size = offset + put;
    WRITES++;
    return (int)put;
}

int vfs_truncate(int id, unsigned size)
{
    if (VFS_READONLY || id < 7 || !vfs_node(id) || size >= NODES[id].size)
        return -1;
    NODES[id].size = size;
    return 0;
}

static int descendants(int id)
{
    int n = 0;
    for (int i = 0; i < NNODES; i++)
        if (NODES[i].used && i != id && NODES[i].parent == id && i != 0)
            n += 1 + descendants(i);
    return n;
}

int vfs_subtree_count(int id) { return descendants(id); }

int vfs_remove(int id)
{
    CHECK(host_uaccess == 0);
    if (VFS_READONLY || !vfs_node(id) || id < 7)
        return -1; /* (the ready-made nodes are not removable here) */
    int count = 1 + descendants(id);
    for (int i = 0; i < NNODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            NODES[i].used = 0;
    NODES[id].used = 0;
    REMOVES++;
    return count;
}

int vfs_rename(int id, const char *name)
{
    const VfsNode *n = vfs_node(id);
    if (VFS_READONLY || !n || !name[0] || strchr(name, '/'))
        return -1;
    int other = child_named(n->parent, name);
    if (other >= 0 && other != id)
        return -1;
    strcpy(NODES[id].name, name);
    return 0;
}

int vfs_move(int id, int dest)
{
    const VfsNode *n = vfs_node(id), *d = vfs_node(dest);
    if (VFS_READONLY || !n || !d || !d->is_dir || child_named(dest, n->name) >= 0)
        return -1;
    NODES[id].parent = dest;
    return id;
}

/* signals and the process list: process.c is not part of this test, so these are fakes that record what the call asked for */
static int SIG_PID, SIG_SIG, SIG_CALLS, SIG_RESULT;
int process_signal(int pid, int sig)
{
    SIG_PID = pid;
    SIG_SIG = sig;
    SIG_CALLS++;
    return SIG_RESULT;
}

/* priorities: process.c is a fake here too; pid 5 and 6 "exist" and start at the default */
static int PRIO_OF[2] = {2, 2}, PRIO_CALLS;
int process_priority(int pid, int prio)
{
    PRIO_CALLS++;
    if (pid < 5 || pid > 6 || prio < -1 || prio > 4)
        return -1;
    int old = PRIO_OF[pid - 5];
    if (prio >= 0)
        PRIO_OF[pid - 5] = prio;
    return old;
}

#define PS_COUNT 3
int process_info(int index, ProcessInfo *out)
{
    if (index < 0 || index >= PS_COUNT)
        return 0;
    out->pid = 10 + index;
    out->ppid = index;
    out->running = index & 1;
    out->console = index - 1;
    snprintf(out->name, sizeof(out->name), "proc%d", index);
    return 1;
}

/* fork / exec / getpid: process.c and exec.c are not part of this test; these fakes record what the calls asked for */
static int FORK_RESULT = 5, FORK_CALLS;
static ull FORK_RIP, FORK_RSP;
int process_fork(ull rip, ull rsp)
{
    FORK_CALLS++;
    FORK_RIP = rip;
    FORK_RSP = rsp;
    return FORK_RESULT;
}

static int EXEC_CALLS, EXEC_NODE, EXEC_ARGC;
static unsigned EXEC_LEN;
static char EXEC_ARGS[PROCESS_ARGS_BYTES], EXEC_NAME[64];
int exec_replace_node(int node, const char *name, ProcessSpawn *args, int *err)
{
    (void)err;
    CHECK(host_uaccess == 0);
    EXEC_CALLS++;
    EXEC_NODE = node;
    strcpy(EXEC_NAME, name);
    EXEC_ARGC = args->argc;
    EXEC_LEN = args->args_len;
    memcpy(EXEC_ARGS, args->args, args->args_len);
    return -5; /* the real one does not return when it works; this fake always "fails" */
}

static int PPID = 77;
int process_ppid(void) { return PPID; }

/* shared memory: the page allocator is real host memory, and the mapping calls only record what was asked */
#include <stdlib.h>
static int PAGES_OUT;
unsigned long long pmm_alloc_page_tag(PmmTag tag)
{
    (void)tag;
    void *p = calloc(1, 4096);
    if (p)
        PAGES_OUT++;
    return (unsigned long long)p;
}
void pmm_free_page(unsigned long long phys)
{
    free((void *)phys);
    PAGES_OUT--;
}

/* the kernel heap (shm.c keeps its page lists there, gui.c its surfaces): host memory, counted, with a switch that makes it fail */
static int HEAP_BLOCKS, HEAP_FAIL;
void *kmalloc(unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *p = calloc(1, size ? (size_t)size : 1);
    if (p)
        HEAP_BLOCKS++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        free(p);
        HEAP_BLOCKS--;
    }
}

#define MAPS_MAX 64
static struct { ull va, phys; unsigned flags; int live; } MAPS[MAPS_MAX];
static int MAPS_N, MAP_FAIL_AT = -1; /* the Nth call of aspace_map_shared fails (-1 = never) */
static int MAP_CALLS;

int aspace_map_shared(AddressSpace *as, ull va, ull phys, unsigned flags)
{
    CHECK(as == &FAKE_AS && (va & 4095) == 0);
    if (MAP_CALLS++ == MAP_FAIL_AT)
        return -1;
    if (MAPS_N < MAPS_MAX)
    {
        MAPS[MAPS_N].va = va;
        MAPS[MAPS_N].phys = phys;
        MAPS[MAPS_N].flags = flags;
        MAPS[MAPS_N].live = 1;
        MAPS_N++;
    }
    return 0;
}

int aspace_unmap_shared(AddressSpace *as, ull va)
{
    CHECK(as == &FAKE_AS);
    for (int i = 0; i < MAPS_N; i++)
        if (MAPS[i].live && MAPS[i].va == va)
        {
            MAPS[i].live = 0;
            return 0;
        }
    return -1;
}

static int maps_live(void)
{
    int n = 0;
    for (int i = 0; i < MAPS_N; i++)
        n += MAPS[i].live;
    return n;
}

/* signal.c is tested on its own (test_signal.c); here the system call side only needs to know what the dispatcher asked of it */
static int SIGDELIV, SIGSYS_CALLS, SIGRET_CALLS;
int signal_deliverable(const Process *p) { (void)p; return SIGDELIV; }
long long signal_syscall_return(SyscallFrame *f, long long result) { (void)f; SIGSYS_CALLS++; return result + 1000; }
long long signal_sigreturn(SyscallFrame *f) { (void)f; SIGRET_CALLS++; return 777; }

/* the volume's lock: volume 0 (RAM Disk) */
static int VOL_LOCKED = 1, VOL_WRITABLE = 1, VOL_LOCKABLE = 1;

int vfs_volume_info(int vol, VfsVolInfo *out)
{
    if (vol != 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->node = 6;
    out->writable = VOL_WRITABLE;
    out->locked = VOL_LOCKED;
    out->lockable = VOL_LOCKABLE;
    return 0;
}

int vfs_set_locked(int vol, int locked)
{
    if (vol != 0 || !VOL_WRITABLE || !VOL_LOCKABLE)
        return -1;
    VOL_LOCKED = locked;
    return 0;
}

int vfs_is_volume_root(int id) { return id == 6; }
int vfs_volume_of(int id)
{
    for (int n = id; n > 0 && n < NNODES; n = NODES[n].parent)
        if (n == 6)
            return 0;
    return -1;
}

/* ---- helpers ---- */
static long long call(ull num, ull a, ull b, ull c)
{
    SyscallFrame f;
    memset(&f, 0, sizeof(f));
    f.rax = num;
    f.rdi = a;
    f.rsi = b;
    f.rdx = c;
    return syscall_dispatch(&f);
}

static void reset(void)
{
    SERIAL_N = 0;
    SERIAL[0] = 0;
    CON_OUT_N = CON_WRITES = 0;
    CON_OUT[0] = 0;
    CON_VALID = 1;
    CON_FINISHED = 0;
    CON_BLOCKS_BEFORE_DATA = 0;
    CON_WAITS = 0;
    CON_WAIT_TASK = -1;
    CON_LINE = "hello\n";
    CON_LINE_POS = 0;
    BLOCKS = YIELDS = 0;
    USER_OK = 1;
    USER_OK_CALLS = 0;
    OK_WRITE = -1;
    BLOCK_FROM = 0;
    CURRENT_AS = &FAKE_AS;
    CURRENT_PROC = &PROC;
    PROC.console = -1;
    PROC.own_console = 1;
    PROC.sig_ignore = 0;
    for (int i = 0; i < PROCESS_MAX_SHM; i++)
        PROC.shm[i].seg = -1;
    PROC.shm_next = 0;
    PROC.stdio[0] = PROC.stdio[1] = PROC.stdio[2] = -1;
    PROC.cwd = 0;
    PROC.parent = -1;
    for (int i = 0; i < PROCESS_MAX_CHILDREN; i++)
        PROC.child[i] = 0;
    for (int i = 0; i < PROCESS_MAX_FILES; i++)
    {
        PROC.file[i].node = -1;
        PROC.file[i].pos = 0;
        PROC.file[i].flags = 0;
    }
    READ_CALLS = 0;
    host_uaccess = 0;
}

/* what "the other process" does while the caller sleeps in task_block (see BLOCK_ACTION) */
static char PIPE_OUT[10000];
static int drained;
static void feed(void) { pipe_write(pfile_pipe(&PROC.file[1]), "late", 4); BLOCK_ACTION = 0; }
static void drain(void) { drained += pipe_read(pfile_pipe(&PROC.file[0]), PIPE_OUT + drained, 3000); }
static void drop_reader(void) { pfile_close(&PROC.file[0]); BLOCK_ACTION = 0; }
static void drop_writer(void) { pfile_close(&PROC.file[1]); BLOCK_ACTION = 0; }

/* what "another process" does while the caller sleeps: lets the mutex go / signals the condition variable (descriptor slots 0 and 1) */
static void mutex_releaser(void) { usync_mutex(pfile_usync(&PROC.file[0]))->owner = -1; BLOCK_ACTION = 0; }
static int SAW_RELEASED;
static void cond_signaller(void)
{
    SAW_RELEASED = usync_mutex(pfile_usync(&PROC.file[0]))->owner == -1; /* the sleeper let the mutex go */
    cond_signal(usync_cond(pfile_usync(&PROC.file[1])));
    BLOCK_ACTION = 0;
}
static void cond_broadcaster(void) { cond_broadcast(usync_cond(pfile_usync(&PROC.file[1]))); BLOCK_ACTION = 0; }
static void sem_poster(void) { sem_post(pfile_sem(&PROC.file[0])); BLOCK_ACTION = 0; }
static void pipe_feed_two(void) { int p = -1; for (int i = 0; i < PROCESS_MAX_FILES; i++) if (PROC.file[i].node >= PROCESS_PIPE_NODE && PROC.file[i].node < PROCESS_MQ_NODE && !(PROC.file[i].flags & PFILE_WRITE)) p = pfile_pipe(&PROC.file[i]); if (p >= 0) pipe_write(p, "ok", 2); BLOCK_ACTION = 0; }
/* what the compositor does while the program sleeps in SYS_GUI_EVENT: the user turns the wheel / closes the window */
static int GUI_TEST_HANDLE;
static void gui_poster(void) { GuiEvent e = {GUI_EV_WHEEL, 3, 0, 0, 0, 0, 0, 0}; gui_post(GUI_TEST_HANDLE, &e); BLOCK_ACTION = 0; }
static void gui_destroyer(void) { gui_destroy(GUI_TEST_HANDLE); BLOCK_ACTION = 0; }
static void raise_int(void) { PROC.sig_pending |= 1u << SIGINT; BLOCK_ACTION = 0; }
static void raise_then_feed(void)
{
    PROC.sig_pending |= 1u << SIGINT;
    BLOCK_ACTION = pipe_feed_two;
}
static void mq_feed(void) { mq_send(pfile_mq(&PROC.file[0]), "fed!", 4); BLOCK_ACTION = 0; }
static void mq_drain(void) { char b[300]; mq_recv(pfile_mq(&PROC.file[0]), b, sizeof(b)); BLOCK_ACTION = 0; }

static ull U(const void *p) { return (ull)(unsigned long long)p; }

int main(void)
{
    setup_vfs();
    char buf[9] = "hi\nthere";

    /* the frame layout is what the assembly pushes: 10 qwords, in this order */
    CHECK(sizeof(SyscallFrame) == 80);
    CHECK(__builtin_offsetof(SyscallFrame, r9) == 0 && __builtin_offsetof(SyscallFrame, rdi) == 40);
    CHECK(__builtin_offsetof(SyscallFrame, rax) == 48 && __builtin_offsetof(SyscallFrame, rcx) == 56);
    CHECK(__builtin_offsetof(SyscallFrame, user_rsp) == 72);

    /* ================= SYS_WRITE ================= */
    /* fd 1 with no window: the serial port, a newline gets a carriage return before it */
    reset();
    ull c0 = syscall_count();
    CHECK(call(SYS_WRITE, 1, U(buf), 8) == 8);
    CHECK(USER_OK_CALLS == 1 && OK_VA == U(buf) && OK_LEN == 8 && OK_WRITE == 0); /* checked for reading */
    CHECK(strcmp(SERIAL, "hi\r\nthere") == 0 && CON_WRITES == 0);
    CHECK(syscall_count() == c0 + 1);
    CHECK(host_uaccess == 0); /* the user-memory window is closed again */

    /* with a console window: the text goes there, not to the serial port; fd 2 (errors) too */
    reset();
    PROC.console = 33;
    CHECK(call(SYS_WRITE, 1, U(buf), 8) == 8);
    CHECK(strcmp(CON_OUT, "hi\nthere") == 0 && SERIAL_N == 0); /* the console wants the raw text: it does \n itself */
    CHECK(call(SYS_WRITE, 2, U("err"), 3) == 3);
    CHECK(strcmp(CON_OUT, "hi\nthereerr") == 0);

    /* a long write goes in pieces, in order, and arrives whole */
    reset();
    PROC.console = 33;
    static char big[SYS_WRITE_MAX];
    for (int i = 0; i < SYS_WRITE_MAX; i++)
        big[i] = (char)('a' + i % 26);
    CHECK(call(SYS_WRITE, 1, U(big), SYS_WRITE_MAX) == SYS_WRITE_MAX);
    CHECK(CON_OUT_N == SYS_WRITE_MAX && memcmp(CON_OUT, big, SYS_WRITE_MAX) == 0 && CON_WRITES > 1);
    CHECK(OK_LEN == SYS_WRITE_MAX);

    /* the window was closed (the handle is no longer valid): the text is dropped, the program is not told */
    reset();
    PROC.console = 33;
    CON_VALID = 0;
    CHECK(call(SYS_WRITE, 1, U(buf), 3) == 3);
    CHECK(CON_OUT_N == 0 && SERIAL_N == 0);

    /* refused: wrong descriptors, a buffer the user may not read, too long, no address space */
    reset();
    CHECK(call(SYS_WRITE, 0, U(buf), 3) == -1 && call(SYS_WRITE, 3, U(buf), 3) == -1);
    CHECK(call(SYS_WRITE, 99, U(buf), 3) == -1 && call(SYS_WRITE, ~0ULL, U(buf), 3) == -1);
    CHECK(USER_OK_CALLS == 0 && SERIAL_N == 0);
    USER_OK = 0;
    CHECK(call(SYS_WRITE, 1, 0x1000, 16) == -1);
    CHECK(USER_OK_CALLS == 1 && SERIAL_N == 0 && host_uaccess == 0); /* the window was never opened */
    reset();
    CHECK(call(SYS_WRITE, 1, U(buf), SYS_WRITE_MAX + 1) == -1 && call(SYS_WRITE, 1, U(buf), ~0ULL) == -1);
    CHECK(USER_OK_CALLS == 0 && SERIAL_N == 0);
    CHECK(call(SYS_WRITE, 1, 0xDEADBEEF, 0) == 0 && USER_OK_CALLS == 0); /* empty: fine, nothing touched */
    CURRENT_AS = 0;
    CHECK(call(SYS_WRITE, 1, U(buf), 3) == -1); /* a kernel thread has no user memory */

    /* ================= SYS_READ: the console (fd 0) ================= */
    reset();
    char in[64];
    CHECK(call(SYS_READ, 0, U(in), sizeof(in)) == 0); /* no window: end of input at once, no waiting */
    CHECK(BLOCKS == 0);

    PROC.console = 33;
    CHECK(call(SYS_READ, 0, U(in), sizeof(in)) == 6 && memcmp(in, "hello\n", 6) == 0); /* a typed line is waiting */
    CHECK(OK_WRITE == 1 && OK_LEN == sizeof(in)); /* the buffer was checked for WRITING */
    CHECK(host_uaccess == 0 && BLOCKS == 0);

    /* nothing typed yet: the thread waits (is blocked) and is woken by Enter; it asks the console to wake it */
    reset();
    PROC.console = 33;
    CON_BLOCKS_BEFORE_DATA = 2;
    CHECK(call(SYS_READ, 0, U(in), sizeof(in)) == 6 && memcmp(in, "hello\n", 6) == 0);
    CHECK(BLOCKS == 2 && CON_WAITS == 2 && CON_WAIT_TASK == 7); /* woke twice (spurious once), then got the line */

    /* a short buffer gets the first part of the line, the rest comes with the next read */
    reset();
    PROC.console = 33;
    CHECK(call(SYS_READ, 0, U(in), 3) == 3 && memcmp(in, "hel", 3) == 0);
    CHECK(call(SYS_READ, 0, U(in), 10) == 3 && memcmp(in, "lo\n", 3) == 0);

    /* the program has ended / the window is gone while it waits: end of input */
    reset();
    PROC.console = 33;
    CON_LINE = "";
    CON_FINISHED = 1;
    CHECK(call(SYS_READ, 0, U(in), 8) == 0);
    reset();
    PROC.console = 33;
    CON_VALID = 0;
    CHECK(call(SYS_READ, 0, U(in), 8) == 0);

    /* bad buffers and lengths */
    reset();
    PROC.console = 33;
    USER_OK = 0;
    CHECK(call(SYS_READ, 0, 0x1000, 8) == -1); /* into the kernel: refused before anything is read or written */
    CHECK(CON_LINE_POS == 0 && host_uaccess == 0);
    USER_OK = 1;
    CHECK(call(SYS_READ, 0, U(in), 0) == 0 && BLOCKS == 0);
    CURRENT_AS = 0;
    CHECK(call(SYS_READ, 0, U(in), 8) == -1);

    /* ================= SYS_OPEN / SYS_READ on files ================= */
    reset();
    static unsigned char data[SYS_READ_MAX];
    long long fd = call(SYS_OPEN, U("HELLO.ELF"), 0, 0);
    CHECK(fd == PROCESS_FD_FIRST && PROC.file[0].node == 1 && PROC.file[0].pos == 0);
    CHECK(call(SYS_OPEN, U("hello.elf"), 0, 0) == PROCESS_FD_FIRST + 1); /* case does not matter; the next slot */
    CHECK(call(SYS_OPEN, U("/RAM Disk/readme.txt"), 0, 0) == PROCESS_FD_FIRST + 2); /* a path from the root */

    /* sequential reads move the position; every byte is the right one, also across the internal 512-byte pieces */
    CHECK(call(SYS_READ, (ull)fd, U(data), 100) == 100 && PROC.file[0].pos == 100);
    for (int i = 0; i < 100; i++)
        CHECK(data[i] == pattern(1, (unsigned)i));
    CHECK(call(SYS_READ, (ull)fd, U(data), 1000) == 500 && PROC.file[0].pos == 600); /* only what is left */
    for (int i = 0; i < 500; i++)
        if (data[i] != pattern(1, 100 + (unsigned)i))
        {
            CHECK(!"wrong byte in a long read");
            break;
        }
    int calls = READ_CALLS;
    CHECK(call(SYS_READ, (ull)fd, U(data), 10) == 0); /* at the end */
    CHECK(READ_CALLS == calls); /* ... decided without asking the drive */
    CHECK(OK_WRITE == 1);

    /* the other descriptor is independent; seeking */
    CHECK(call(SYS_READ, (ull)fd + 1, U(data), 4) == 4 && data[0] == pattern(1, 0) && PROC.file[1].pos == 4);
    CHECK(call(SYS_SEEK, (ull)fd, 0, 0) == 0 && PROC.file[0].pos == 0);
    CHECK(call(SYS_SEEK, (ull)fd, 10, 0) == 10);
    CHECK(call(SYS_SEEK, (ull)fd, 5, 1) == 15);
    CHECK(call(SYS_SEEK, (ull)fd, (ull)(long long)-5, 1) == 10);
    CHECK(call(SYS_SEEK, (ull)fd, 0, 2) == 600);                         /* the size */
    CHECK(call(SYS_SEEK, (ull)fd, (ull)(long long)-100, 2) == 500);
    CHECK(call(SYS_SEEK, (ull)fd, 100, 2) == 700);                       /* past the end is allowed */
    CHECK(call(SYS_READ, (ull)fd, U(data), 10) == 0);                    /* ... and reads nothing */
    CHECK(call(SYS_SEEK, (ull)fd, 0, 0) == 0);
    CHECK(call(SYS_READ, (ull)fd, U(data), 3) == 3 && data[2] == pattern(1, 2));

    /* seek that must be refused, and leaves the position as it was */
    CHECK(call(SYS_SEEK, (ull)fd, 0, 0) == 0);
    CHECK(call(SYS_SEEK, (ull)fd, (ull)(long long)-1, 0) == -1);          /* before the start */
    CHECK(call(SYS_SEEK, (ull)fd, (ull)(long long)-601, 2) == -1);
    CHECK(call(SYS_SEEK, (ull)fd, 0, 3) == -1 && call(SYS_SEEK, (ull)fd, 0, ~0ULL) == -1); /* no such whence */
    CHECK(call(SYS_SEEK, (ull)fd, 0x7FFFFFFFFFFFFFFFULL, 2) == -1);       /* would overflow */
    CHECK(call(SYS_SEEK, (ull)fd, 0x7FFFFFFFFFFFFFFFULL, 1) == -1);
    CHECK(call(SYS_SEEK, (ull)fd, 0x100000000ULL, 0) == -1);              /* does not fit a 32-bit position */
    CHECK(call(SYS_SEEK, (ull)fd, 0xFFFFFFFFULL, 0) == 0xFFFFFFFFLL);     /* the largest position that does */
    CHECK(call(SYS_SEEK, (ull)fd, 0, 0) == 0);
    CHECK(PROC.file[0].pos == 0);

    /* a drive error half way: the bytes already read are returned, then the error */
    int bad = (int)call(SYS_OPEN, U("BAD.BIN"), 0, 0);
    CHECK(bad >= 3);
    CHECK(call(SYS_READ, (ull)bad, U(data), 1500) == 1024); /* up to the bad place */
    CHECK(call(SYS_READ, (ull)bad, U(data), 100) == -1);    /* now nothing is readable */
    CHECK(call(SYS_CLOSE, (ull)bad, 0, 0) == 0);

    /* an empty file reads 0; a bad buffer is refused without touching the drive or the position */
    int empty = (int)call(SYS_OPEN, U("EMPTY.BIN"), 0, 0);
    CHECK(empty >= 3 && call(SYS_READ, (ull)empty, U(data), 10) == 0);
    call(SYS_CLOSE, (ull)empty, 0, 0);
    calls = READ_CALLS;
    USER_OK = 0;
    CHECK(call(SYS_READ, (ull)fd, 0x1000, 10) == -1 && READ_CALLS == calls && PROC.file[0].pos == 0);
    USER_OK = 1;

    /* a read of the maximum size is accepted (and a bigger one is shortened, not refused) */
    CHECK(call(SYS_SEEK, (ull)fd, 0, 0) == 0);
    CHECK(call(SYS_READ, (ull)fd, U(data), SYS_READ_MAX * 4) == 600 && OK_LEN == SYS_READ_MAX);

    /* ================= SYS_OPEN: what is refused ================= */
    reset();
    CHECK(call(SYS_OPEN, U("NO-SUCH.FILE"), 0, 0) == -1);
    CHECK(call(SYS_OPEN, U("DIR"), 0, 0) == -1);                        /* a folder is not a file */
    CHECK(call(SYS_OPEN, U("/RAM Disk/DIR"), 0, 0) == -1);
    CHECK(call(SYS_OPEN, U("/nothing"), 0, 0) == -1);
    CHECK(call(SYS_OPEN, U("HELLO.ELF"), 1, 0) == -1);                  /* only O_RDONLY (0) exists */
    CHECK(call(SYS_OPEN, U(""), 0, 0) == -1);
    CHECK(PROC.file[0].node == -1);                                     /* nothing was opened by all of that */
    USER_OK = 0;
    CHECK(call(SYS_OPEN, 0x1000, 0, 0) == -1);                          /* a path pointing into the kernel */
    CHECK(host_uaccess == 0);
    USER_OK = 1;
    CURRENT_AS = 0;
    CHECK(call(SYS_OPEN, U("HELLO.ELF"), 0, 0) == -1);
    CURRENT_AS = &FAKE_AS;

    /* a path that is not terminated: refused, not read past its buffer */
    static char longpath[400];
    memset(longpath, 'A', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = 0;
    CHECK(call(SYS_OPEN, U(longpath), 0, 0) == -1);
    memset(longpath, 'B', 255);
    longpath[255] = 0;                                                    /* 255 characters: the longest that fits */
    CHECK(call(SYS_OPEN, U(longpath), 0, 0) == -1);                       /* (fits, but there is no such file) */
    CHECK(host_uaccess == 0);

    /* a path that runs into an unmapped page: the second page is checked when the string gets there */
    static char pages[8192] __attribute__((aligned(4096)));
    strcpy(pages + 4096 - 4, "HEL"); /* "HEL" then NUL at the end of the first page: complete, no second page needed */
    BLOCK_FROM = U(pages) + 4096;
    CHECK(call(SYS_OPEN, U(pages + 4096 - 4), 0, 0) == -1); /* no file called HEL, but no refusal for the page either */
    int before = USER_OK_CALLS;
    memcpy(pages + 4096 - 4, "HELL", 4);                    /* no NUL before the page ends: it continues on page 2 */
    pages[4096] = 'O';
    pages[4097] = 0;
    CHECK(call(SYS_OPEN, U(pages + 4096 - 4), 0, 0) == -1); /* page 2 is not mapped: refused */
    CHECK(USER_OK_CALLS == before + 2);                     /* once for the start, once when it reached the new page */
    BLOCK_FROM = 0;
    CHECK(host_uaccess == 0);

    /* only PROCESS_MAX_FILES descriptors: the next open fails; closing makes room (the lowest free number comes first) */
    reset();
    int fds[PROCESS_MAX_FILES + 2];
    for (int i = 0; i < PROCESS_MAX_FILES; i++)
    {
        fds[i] = (int)call(SYS_OPEN, U("HELLO.ELF"), 0, 0);
        CHECK(fds[i] == PROCESS_FD_FIRST + i);
    }
    CHECK(call(SYS_OPEN, U("HELLO.ELF"), 0, 0) == -1);
    CHECK(call(SYS_CLOSE, (ull)fds[2], 0, 0) == 0);
    CHECK(call(SYS_OPEN, U("HELLO.ELF"), 0, 0) == fds[2]);

    /* ================= SYS_CLOSE ================= */
    CHECK(call(SYS_CLOSE, (ull)fds[0], 0, 0) == 0);
    CHECK(call(SYS_CLOSE, (ull)fds[0], 0, 0) == -1); /* twice */
    CHECK(call(SYS_READ, (ull)fds[0], U(data), 4) == -1 && call(SYS_SEEK, (ull)fds[0], 0, 0) == -1); /* closed: unusable */
    CHECK(call(SYS_CLOSE, 99, 0, 0) == -1 && call(SYS_CLOSE, ~0ULL, 0, 0) == -1);
    CHECK(call(SYS_CLOSE, 0, 0, 0) == 0 && call(SYS_CLOSE, 1, 0, 0) == 0 && call(SYS_CLOSE, 2, 0, 0) == 0); /* the console: no-op */

    /* descriptors 0..2 are not files */
    CHECK(call(SYS_SEEK, 0, 0, 0) == -1 && call(SYS_SEEK, 1, 0, 0) == -1);
    CHECK(call(SYS_READ, 1, U(in), 4) == -1 && call(SYS_READ, 2, U(in), 4) == -1);

    /* a kernel thread (no Process record) can use none of it */
    CURRENT_PROC = 0;
    CHECK(call(SYS_OPEN, U("HELLO.ELF"), 0, 0) == -1);
    CHECK(call(SYS_CLOSE, 3, 0, 0) == -1 && call(SYS_SEEK, 3, 0, 0) == -1 && call(SYS_READ, 3, U(in), 4) == -1);
    CHECK(call(SYS_READ, 0, U(in), 4) == -1);
    CURRENT_PROC = &PROC;

    /* the file was removed while open: a read says so instead of using a stale node */
    reset();
    fd = call(SYS_OPEN, U("HELLO.ELF"), 0, 0);
    NODES[1].used = 0;
    PROC.file[0].node = 99; /* a node number that no longer exists */
    CHECK(call(SYS_READ, (ull)fd, U(data), 4) == -1 && call(SYS_SEEK, (ull)fd, 0, 2) == -1);
    NODES[1].used = 1;

    /* ================= brk ================= */
    reset();
    const ull HB = 0x400000010000ULL;
    PROC.heap_base = PROC.heap_brk = PROC.heap_mapped = HB;
    PROC.heap_limit = HB + 16 * 4096;
    MAPPED_PAGES = 0;
    CHECK(call(SYS_BRK, 0, 0, 0) == (long long)HB);                               /* 0 = ask */
    CHECK(call(SYS_BRK, HB + 100, 0, 0) == (long long)(HB + 100) && MAPPED_PAGES == 1);
    CHECK(call(SYS_BRK, HB + 4096, 0, 0) == (long long)(HB + 4096) && MAPPED_PAGES == 1); /* same page */
    CHECK(call(SYS_BRK, HB + 4097, 0, 0) == (long long)(HB + 4097) && MAPPED_PAGES == 2 && LAST_MAPPED_VA == HB + 4096);
    CHECK(call(SYS_BRK, HB, 0, 0) == (long long)HB && MAPPED_PAGES == 2);                /* lowering maps nothing */
    CHECK(call(SYS_BRK, HB + 8192, 0, 0) == (long long)(HB + 8192) && MAPPED_PAGES == 2); /* still mapped: not again */
    CHECK(call(SYS_BRK, HB - 1, 0, 0) == -1 && call(SYS_BRK, HB + 16 * 4096 + 1, 0, 0) == -1);
    CHECK(call(SYS_BRK, ~0ULL, 0, 0) == -1 && call(SYS_BRK, 1, 0, 0) == -1);
    CHECK(call(SYS_BRK, 0, 0, 0) == (long long)(HB + 8192) && MAPPED_PAGES == 2);        /* refused: nothing moved */
    CHECK(call(SYS_BRK, HB + 16 * 4096, 0, 0) == (long long)(HB + 16 * 4096) && MAPPED_PAGES == 16); /* the limit itself */
    /* out of memory half way: the break stays, the pages already mapped stay counted, a retry continues */
    PROC.heap_brk = PROC.heap_mapped = HB;
    MAPPED_PAGES = 0;
    MAP_FAIL_AFTER = 3;
    CHECK(call(SYS_BRK, HB + 5 * 4096, 0, 0) == -1 && MAPPED_PAGES == 2 && PROC.heap_brk == HB && PROC.heap_mapped == HB + 2 * 4096);
    CHECK(call(SYS_BRK, HB + 5 * 4096, 0, 0) == (long long)(HB + 5 * 4096) && MAPPED_PAGES == 5);
    CURRENT_PROC = 0;
    CHECK(call(SYS_BRK, 0, 0, 0) == -1);                                                 /* a kernel thread has no heap */
    CURRENT_PROC = &PROC;

    /* ================= spawn and wait ================= */
    reset();
    PROC.console = 4;
    SPAWN_CALLS = 0;
    CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) == 100);               /* no argv: the path is argv[0] */
    CHECK(SPAWN_CALLS == 1 && SP_NODE == 1 && strcmp(SP_NAME, "HELLO.ELF") == 0);
    CHECK(SP_ARGC == 1 && SP_LEN == 10 && strcmp(SP_ARGS, "HELLO.ELF") == 0);
    CHECK(SP_CONSOLE == 4 && SP_PARENT == 7);                          /* shares the console; parent = this thread */
    CHECK(PROC.child[0] == 100 && PROC.child[1] == 0);
    {
        const char *argv[] = {"one", "two words", 0};
        CHECK(call(SYS_SPAWN, U("hello.elf"), U(argv), 0) == 101);     /* the case of the path does not matter */
        CHECK(SP_ARGC == 2 && SP_LEN == 14 && memcmp(SP_ARGS, "one\0two words\0", 14) == 0);
        CHECK(PROC.child[1] == 101);
        const char *none[] = {0};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), U(none), 0) == 102);     /* an empty argv: argc 0 */
        CHECK(SP_ARGC == 0 && SP_LEN == 0);
        const char *many[40];
        for (int i = 0; i < 39; i++)
            many[i] = "a";
        many[39] = 0;
        int calls = SPAWN_CALLS;
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), U(many), 0) == -1 && SPAWN_CALLS == calls); /* over PROCESS_ARGS_MAX */
        static char longarg[2100];
        memset(longarg, 'z', sizeof(longarg) - 1);
        const char *big[] = {longarg, 0};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), U(big), 0) == -1 && SPAWN_CALLS == calls); /* over PROCESS_ARGS_BYTES */
        const char *four[] = {"a", "b", "c", "d", 0};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), U(four), 0) == 103 && PROC.child[3] == 103); /* the 4th slot */
    }
    CHECK(call(SYS_SPAWN, U("NO-SUCH.ELF"), 0, 0) == -1);
    CHECK(call(SYS_SPAWN, U("/RAM Disk/DIR"), 0, 0) == -1);           /* a folder */
    CHECK(call(SYS_SPAWN, U(""), 0, 0) == -1);
    USER_OK = 0;
    CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) == -1);               /* bad path pointer */
    USER_OK = 1;
    SPAWN_FAIL = 1;
    CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) == -1 && PROC.child[4] == 0); /* the loader said no: no slot taken */
    SPAWN_FAIL = 0;
    for (int i = 0; i < PROCESS_MAX_CHILDREN; i++)
        PROC.child[i] = 900 + i;
    {
        int calls = SPAWN_CALLS;
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) == -1 && SPAWN_CALLS == calls); /* all slots used: nothing is started */
    }
    CURRENT_PROC = 0;
    CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) == -1 && call(SYS_WAIT, 100, 0, 0) == -1); /* kernel thread */
    CURRENT_PROC = &PROC;

    /* wait: the child ends while we sleep */
    for (int i = 0; i < PROCESS_MAX_CHILDREN; i++)
        PROC.child[i] = 0;
    PROC.child[2] = 200;
    PENDING_PID = 200;
    PENDING_CODE = 42;
    int st = 0;
    BLOCKS = 0;
    CHECK(call(SYS_WAIT, 200, U(&st), 0) == 200 && st == 42 && BLOCKS == 1 && PROC.child[2] == 0);
    CHECK(call(SYS_WAIT, 200, U(&st), 0) == -1);                       /* once only */
    PROC.child[2] = 201;
    DONE_PID = 201;                                                    /* already ended: no sleep at all */
    DONE_CODE = -1;
    BLOCKS = 0;
    CHECK(call(SYS_WAIT, 201, U(&st), 0) == 201 && st == -1 && BLOCKS == 0);
    PROC.child[2] = 202;
    DONE_PID = 202;
    DONE_CODE = 9;
    CHECK(call(SYS_WAIT, 202, 0, 0) == 202);                           /* no place for the code is fine */
    PROC.child[0] = 300;
    CHECK(call(SYS_WAIT, 301, U(&st), 0) == -1 && call(SYS_WAIT, 0, U(&st), 0) == -1); /* not our child */
    CHECK(call(SYS_WAIT, ~0ULL, U(&st), 0) == -1 && call(SYS_WAIT, 0x100000000ULL + 300, U(&st), 0) == -1);
    DONE_PID = 300;
    DONE_CODE = 5;
    USER_OK = 0;
    CHECK(call(SYS_WAIT, 300, U(&st), 0) == -1 && PROC.child[0] == 300); /* bad pointer: the child can still be waited for */
    USER_OK = 1;
    CHECK(call(SYS_WAIT, 300, U(&st), 0) == 300 && st == 5);
    CURRENT_PROC = &PROC;

    /* ================= folders: readdir, stat, chdir, getcwd, relative paths ================= */
    reset();
    {
        SysDirEntry e;
        memset(&e, 0x55, sizeof(e));
        CHECK(call(SYS_READDIR, U("/"), 0, U(&e)) == 1 && strcmp(e.name, "RAM Disk") == 0 && e.is_dir == 1 && e.size == 0);
        CHECK(call(SYS_READDIR, U("/"), 1, U(&e)) == 0);                                  /* past the end */
        CHECK(call(SYS_READDIR, U("/RAM Disk"), 0, U(&e)) == 1 && strcmp(e.name, "DIR") == 0 && e.is_dir == 1);
        CHECK(call(SYS_READDIR, U("/RAM Disk"), 1, U(&e)) == 1 && strcmp(e.name, "readme.txt") == 0 && e.size == 20 && e.is_dir == 0);
        CHECK(e.name[11] == 0 && e.name[95] == 0);                                        /* the rest of the name is cleared */
        CHECK(call(SYS_READDIR, U("/RAM Disk"), 2, U(&e)) == 0);
        CHECK(call(SYS_READDIR, U("/RAM Disk/DIR"), 0, U(&e)) == 0);                      /* an empty folder */
        CHECK(call(SYS_READDIR, U("/RAM Disk/readme.txt"), 0, U(&e)) == -1);              /* a file */
        CHECK(call(SYS_READDIR, U("/nothing"), 0, U(&e)) == -1);
        CHECK(call(SYS_READDIR, U("/RAM Disk"), 512, U(&e)) == -1 && call(SYS_READDIR, U("/RAM Disk"), ~0ULL, U(&e)) == -1);
        USER_OK = 0;
        CHECK(call(SYS_READDIR, U("/RAM Disk"), 0, U(&e)) == -1);                         /* bad pointers */
        USER_OK = 1;

        CHECK(call(SYS_STAT, U("/RAM Disk/readme.txt"), U(&e), 0) == 0 && e.size == 20 && e.is_dir == 0 && strcmp(e.name, "readme.txt") == 0);
        CHECK(call(SYS_STAT, U("/"), U(&e), 0) == 0 && e.is_dir == 1 && e.name[0] == 0);
        CHECK(call(SYS_STAT, U("HELLO.ELF"), U(&e), 0) == 0 && e.size == 600);              /* a plain name: the programs' folder */
        CHECK(call(SYS_STAT, U("nothing"), U(&e), 0) == -1);
        USER_OK = 0;
        CHECK(call(SYS_STAT, U("/"), U(&e), 0) == -1);
        USER_OK = 1;

        /* the working folder */
        char cwd[32];
        CHECK(call(SYS_GETCWD, U(cwd), sizeof(cwd), 0) == 1 && strcmp(cwd, "/") == 0);
        CHECK(call(SYS_CHDIR, U("/RAM Disk"), 0, 0) == 0 && PROC.cwd == 6);
        CHECK(call(SYS_GETCWD, U(cwd), sizeof(cwd), 0) == 9 && strcmp(cwd, "/RAM Disk") == 0);
        CHECK(call(SYS_GETCWD, U(cwd), 9, 0) == -1 && call(SYS_GETCWD, U(cwd), 10, 0) == 9); /* the NUL needs room too */
        CHECK(call(SYS_GETCWD, U(cwd), 0, 0) == -1);
        USER_OK = 0;
        CHECK(call(SYS_GETCWD, U(cwd), sizeof(cwd), 0) == -1);
        USER_OK = 1;
        CHECK(call(SYS_STAT, U("readme.txt"), U(&e), 0) == 0 && e.size == 20);             /* relative to the folder */
        CHECK(call(SYS_STAT, U("DIR/../readme.txt"), U(&e), 0) == 0 && e.size == 20);
        CHECK(call(SYS_STAT, U("./readme.txt"), U(&e), 0) == 0 && call(SYS_STAT, U(".//DIR/."), U(&e), 0) == 0 && e.is_dir);
        CHECK(call(SYS_CHDIR, U("DIR"), 0, 0) == 0 && PROC.cwd == 2);
        CHECK(call(SYS_GETCWD, U(cwd), sizeof(cwd), 0) == 13 && strcmp(cwd, "/RAM Disk/DIR") == 0);
        CHECK(call(SYS_CHDIR, U(".."), 0, 0) == 0 && PROC.cwd == 6);
        CHECK(call(SYS_CHDIR, U("../.."), 0, 0) == 0 && PROC.cwd == 0);
        CHECK(call(SYS_CHDIR, U("/RAM Disk/DIR"), 0, 0) == 0 && call(SYS_CHDIR, U("../../../.."), 0, 0) == 0 && PROC.cwd == 0); /* never above the root */
        CHECK(call(SYS_CHDIR, U("/RAM Disk"), 0, 0) == 0);
        CHECK(call(SYS_CHDIR, U("readme.txt"), 0, 0) == -1 && PROC.cwd == 6);               /* a file */
        CHECK(call(SYS_CHDIR, U("nothing"), 0, 0) == -1 && call(SYS_CHDIR, U("HELLO.ELF"), 0, 0) == -1 && PROC.cwd == 6);
        USER_OK = 0;
        CHECK(call(SYS_CHDIR, U("/"), 0, 0) == -1 && PROC.cwd == 6);
        USER_OK = 1;

        /* open and spawn use it: a name in the folder wins, otherwise the programs' folder; a child inherits the folder */
        long long fd = call(SYS_OPEN, U("readme.txt"), 0, 0);
        CHECK(fd >= 3 && PROC.file[fd - 3].node == 3);
        call(SYS_CLOSE, (ull)fd, 0, 0);
        fd = call(SYS_OPEN, U("DIR/../readme.txt"), 0, 0);
        CHECK(fd >= 3 && PROC.file[fd - 3].node == 3);
        call(SYS_CLOSE, (ull)fd, 0, 0);
        fd = call(SYS_OPEN, U("HELLO.ELF"), 0, 0);
        CHECK(fd >= 3 && PROC.file[fd - 3].node == 1);
        call(SYS_CLOSE, (ull)fd, 0, 0);
        CHECK(call(SYS_OPEN, U("DIR"), 0, 0) == -1);                                       /* a folder cannot be opened */
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) > 0 && SP_NODE == 1 && SP_CWD == 6);
        PROC.child[0] = 0;
        PROC.cwd = 99; /* the folder's drive was unplugged: it is the top again */
        CHECK(call(SYS_GETCWD, U(cwd), sizeof(cwd), 0) == 1 && strcmp(cwd, "/") == 0 && PROC.cwd == 0);
        CHECK(call(SYS_STAT, U("DIR"), U(&e), 0) == -1);                                  /* the top has no DIR */

        CURRENT_PROC = 0;
        CHECK(call(SYS_READDIR, U("/"), 0, U(&e)) == -1 && call(SYS_STAT, U("/"), U(&e), 0) == -1);
        CHECK(call(SYS_CHDIR, U("/"), 0, 0) == -1 && call(SYS_GETCWD, U(cwd), sizeof(cwd), 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= writing files, mkdir, unlink, rmdir, rename, redirection ================= */
    reset();
    {
        SysDirEntry e;
        memset(&e, 0, sizeof(e));
        char rb[64];
        CHECK(call(SYS_MKDIR, U("/RAM Disk/W"), 0, 0) == 0);
        CHECK(call(SYS_STAT, U("/RAM Disk/W"), U(&e), 0) == 0 && e.is_dir == 1);
        CHECK(call(SYS_MKDIR, U("/RAM Disk/W"), 0, 0) == -1);                 /* exists */
        CHECK(call(SYS_MKDIR, U("/RAM Disk/readme.txt"), 0, 0) == -1);        /* a file is there */
        CHECK(call(SYS_MKDIR, U("/RAM Disk/W/"), 0, 0) == -1);                /* exists, with a trailing slash */
        CHECK(call(SYS_MKDIR, U("/RAM Disk/no/such"), 0, 0) == -1);           /* the folder part is missing */
        CHECK(call(SYS_MKDIR, U("/Top"), 0, 0) == -1 && call(SYS_MKDIR, U("Top"), 0, 0) == -1); /* nothing but drives in "This PC" */
        CHECK(call(SYS_MKDIR, U(""), 0, 0) == -1 && call(SYS_MKDIR, U("/RAM Disk/."), 0, 0) == -1);
        VFS_READONLY = 1;
        CHECK(call(SYS_MKDIR, U("/RAM Disk/X"), 0, 0) == -1);                 /* a locked drive */
        VFS_READONLY = 0;
        USER_OK = 0;
        CHECK(call(SYS_MKDIR, U("/RAM Disk/Y"), 0, 0) == -1);                 /* bad pointer */
        USER_OK = 1;

        CHECK(call(SYS_CHDIR, U("/RAM Disk/W"), 0, 0) == 0);

        /* create, write, read back */
        CHECK(call(SYS_OPEN, U("f.txt"), 0, 0) == -1);                        /* there is no such file */
        long long fd = call(SYS_OPEN, U("f.txt"), SYS_O_WRITE | SYS_O_CREATE, 0);
        CHECK(fd >= 3 && PROC.file[fd - 3].flags == PFILE_WRITE);
        CHECK(call(SYS_WRITE, (ull)fd, U("hello"), 5) == 5 && call(SYS_WRITE, (ull)fd, U(" world"), 6) == 6);
        CHECK(call(SYS_WRITE, (ull)fd, U("x"), 0) == 0);
        CHECK(call(SYS_READ, (ull)fd, U(rb), 4) == -1);                        /* opened for writing only */
        CHECK(call(SYS_SEEK, (ull)fd, 0, 2) == 11);
        CHECK(call(SYS_STAT, U("f.txt"), U(&e), 0) == 0 && e.size == 11 && !e.is_dir);
        CHECK(call(SYS_CLOSE, (ull)fd, 0, 0) == 0);
        fd = call(SYS_OPEN, U("f.txt"), 0, 0);
        memset(rb, 0, sizeof(rb));
        CHECK(fd >= 3 && call(SYS_READ, (ull)fd, U(rb), 32) == 11 && strcmp(rb, "hello world") == 0);
        CHECK(call(SYS_WRITE, (ull)fd, U("zz"), 2) == -1);                     /* opened for reading */
        call(SYS_CLOSE, (ull)fd, 0, 0);

        /* flags */
        CHECK(call(SYS_OPEN, U("f.txt"), 16, 0) == -1);                        /* no such flag */
        CHECK(call(SYS_OPEN, U("f.txt"), SYS_O_CREATE, 0) == -1 && call(SYS_OPEN, U("f.txt"), SYS_O_TRUNC, 0) == -1 &&
              call(SYS_OPEN, U("f.txt"), SYS_O_APPEND, 0) == -1);              /* they need WRITE */
        fd = call(SYS_OPEN, U("f.txt"), SYS_O_WRITE | SYS_O_CREATE, 0);        /* existing: kept, position 0 */
        CHECK(fd >= 3 && call(SYS_STAT, U("f.txt"), U(&e), 0) == 0 && e.size == 11);
        CHECK(call(SYS_WRITE, (ull)fd, U("J"), 1) == 1);                       /* overwrites the first byte */
        call(SYS_CLOSE, (ull)fd, 0, 0);
        fd = call(SYS_OPEN, U("f.txt"), SYS_O_WRITE | SYS_O_APPEND, 0);
        CHECK(fd >= 3 && call(SYS_WRITE, (ull)fd, U("!"), 1) == 1 && call(SYS_WRITE, (ull)fd, U("?"), 1) == 1);
        call(SYS_CLOSE, (ull)fd, 0, 0);
        fd = call(SYS_OPEN, U("f.txt"), 0, 0);
        memset(rb, 0, sizeof(rb));
        CHECK(call(SYS_READ, (ull)fd, U(rb), 32) == 13 && strcmp(rb, "Jello world!?") == 0);
        call(SYS_CLOSE, (ull)fd, 0, 0);
        fd = call(SYS_OPEN, U("f.txt"), SYS_O_WRITE | SYS_O_TRUNC, 0);
        CHECK(fd >= 3 && call(SYS_STAT, U("f.txt"), U(&e), 0) == 0 && e.size == 0);
        CHECK(call(SYS_WRITE, (ull)fd, U("xy"), 2) == 2);
        call(SYS_CLOSE, (ull)fd, 0, 0);

        /* what cannot be opened for writing / created */
        CHECK(call(SYS_OPEN, U("/RAM Disk/W"), SYS_O_WRITE, 0) == -1);         /* a folder */
        CHECK(call(SYS_OPEN, U("/nodir/f"), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);
        CHECK(call(SYS_OPEN, U("/f"), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);   /* "This PC" holds drives only */
        CHECK(call(SYS_OPEN, U("a/b"), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);
        CHECK(call(SYS_OPEN, U("dir/"), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);
        CHECK(call(SYS_OPEN, U(".."), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);
        char longname[120];
        memset(longname, 'n', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = 0;
        CHECK(call(SYS_OPEN, U(longname), SYS_O_WRITE | SYS_O_CREATE, 0) == -1); /* longer than a VFS name */
        VFS_READONLY = 1;
        CHECK(call(SYS_OPEN, U("new.txt"), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);
        fd = call(SYS_OPEN, U("f.txt"), SYS_O_WRITE, 0);
        CHECK(fd >= 3 && call(SYS_WRITE, (ull)fd, U("q"), 1) == -1);           /* the drive refuses the write */
        CHECK(call(SYS_OPEN, U("f.txt"), SYS_O_WRITE | SYS_O_TRUNC, 0) == -1);
        VFS_READONLY = 0;
        call(SYS_CLOSE, (ull)fd, 0, 0);

        /* a plain name for writing is the working folder, never the programs' folder */
        fd = call(SYS_OPEN, U("HELLO.ELF"), SYS_O_WRITE | SYS_O_CREATE, 0);
        CHECK(fd >= 3 && PROC.file[fd - 3].node != 1 && PROC.file[fd - 3].node >= 7);
        call(SYS_CLOSE, (ull)fd, 0, 0);
        CHECK(call(SYS_UNLINK, U("HELLO.ELF"), 0, 0) == 0);

        /* a full drive: a short write, then nothing */
        fd = call(SYS_OPEN, U("big"), SYS_O_WRITE | SYS_O_CREATE, 0);
        static char blk[4096];
        memset(blk, 'b', sizeof(blk));
        CHECK(fd >= 3 && call(SYS_WRITE, (ull)fd, U(blk), 4000) == 4000);
        CHECK(call(SYS_WRITE, (ull)fd, U(blk), 200) == 96);                    /* only 96 bytes were left */
        CHECK(call(SYS_WRITE, (ull)fd, U(blk), 10) == -1);
        call(SYS_CLOSE, (ull)fd, 0, 0);

        /* redirection: a file standing in for fd 1 and fd 0 */
        reset();
        PROC.console = 4;
        CHECK(call(SYS_CHDIR, U("/RAM Disk/W"), 0, 0) == 0);
        long long out = call(SYS_OPEN, U("out.txt"), SYS_O_WRITE | SYS_O_CREATE, 0);
        long long in = call(SYS_OPEN, U("f.txt"), 0, 0);
        CHECK(out >= 3 && in >= 3);
        PROC.stdio[1] = (int)out - 3;
        PROC.stdio[0] = (int)in - 3;
        CON_OUT_N = 0;
        CHECK(call(SYS_WRITE, 1, U("to file"), 7) == 7 && CON_OUT_N == 0 && CON_WRITES == 0);
        CHECK(call(SYS_STAT, U("out.txt"), U(&e), 0) == 0 && e.size == 7);
        CHECK(call(SYS_READ, 0, U(rb), 2) == 2 && rb[0] == 'x' && rb[1] == 'y');   /* fd 0 is f.txt now */
        CHECK(call(SYS_WRITE, 2, U("!"), 1) == 1 && CON_WRITES == 1);                /* fd 2 is still the console */
        CHECK(call(SYS_CLOSE, (ull)out, 0, 0) == 0 && PROC.stdio[1] == -1);         /* closing puts the console back */
        CHECK(call(SYS_WRITE, 1, U("back"), 4) == 4 && CON_WRITES == 2);
        PROC.stdio[0] = -1;
        call(SYS_CLOSE, (ull)in, 0, 0);

        /* SYS_SPAWN with io: the caller's descriptors become the child's fd 0 / 1 / 2 */
        out = call(SYS_OPEN, U("out2.txt"), SYS_O_WRITE | SYS_O_CREATE, 0);
        in = call(SYS_OPEN, U("f.txt"), 0, 0);
        int io[3] = {(int)in, (int)out, (int)out};
        SP_REDIR_N = 0;
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io)) > 0 && SP_REDIR_N == 3);
        CHECK(SP_REDIR[0].node == PROC.file[in - 3].node && !(SP_REDIR[0].flags & PFILE_WRITE));
        CHECK(SP_REDIR[1].node == PROC.file[out - 3].node && (SP_REDIR[1].flags & PFILE_WRITE) && SP_REDIR[2].node == SP_REDIR[1].node);
        PROC.child[0] = 0;
        int only_out[3] = {-1, (int)out, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(only_out)) > 0 && SP_REDIR[0].node < 0 && SP_REDIR[1].node >= 7 && SP_REDIR[2].node < 0);
        PROC.child[0] = 0;
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U((int[3]){-1, -1, -1})) > 0 && SP_REDIR_N == 0); /* nothing redirected */
        PROC.child[0] = 0;
        PROC.stdio[1] = (int)out - 3; /* the caller is redirected itself: the child inherits it */
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0) > 0 && SP_REDIR_N == 3 && SP_REDIR[1].node >= 7 && SP_REDIR[0].node < 0);
        PROC.child[0] = 0;
        PROC.stdio[1] = -1;
        int wrong_way[3] = {(int)out, -1, -1}; /* an output file as input */
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(wrong_way)) == -1);
        int wrong_way2[3] = {-1, (int)in, -1}; /* an input file as output */
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(wrong_way2)) == -1);
        int bad_fd[3] = {-1, 2, -1}, closed_fd[3] = {-1, 7, -1}, far_fd[3] = {-1, 99, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(bad_fd)) == -1 && call(SYS_SPAWN, U("HELLO.ELF"), 0, U(closed_fd)) == -1 &&
              call(SYS_SPAWN, U("HELLO.ELF"), 0, U(far_fd)) == -1);
        USER_OK = 0;
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, 0x1000) == -1);               /* bad pointer for io */
        USER_OK = 1;
        call(SYS_CLOSE, (ull)out, 0, 0);
        call(SYS_CLOSE, (ull)in, 0, 0);

        /* rename and move */
        reset();
        CHECK(call(SYS_CHDIR, U("/RAM Disk/W"), 0, 0) == 0);
        CHECK(call(SYS_RENAME, U("f.txt"), U("g.txt"), 0) == 0 && call(SYS_STAT, U("g.txt"), U(&e), 0) == 0 && e.size == 2);
        CHECK(call(SYS_STAT, U("f.txt"), U(&e), 0) == -1);
        CHECK(call(SYS_RENAME, U("g.txt"), U("out.txt"), 0) == -1);              /* the name is taken */
        CHECK(call(SYS_RENAME, U("g.txt"), U("G.TXT"), 0) == 0 && call(SYS_STAT, U("g.txt"), U(&e), 0) == 0);
        CHECK(call(SYS_RENAME, U("nothing"), U("x"), 0) == -1);
        CHECK(call(SYS_RENAME, U("g.txt"), U(""), 0) == -1 && call(SYS_RENAME, U("g.txt"), U("a/"), 0) == -1);
        CHECK(call(SYS_MKDIR, U("/RAM Disk/W2"), 0, 0) == 0);
        CHECK(call(SYS_RENAME, U("g.txt"), U("/RAM Disk/W2/g.txt"), 0) == 0);    /* a move */
        CHECK(call(SYS_STAT, U("/RAM Disk/W2/g.txt"), U(&e), 0) == 0 && e.size == 2 && call(SYS_STAT, U("g.txt"), U(&e), 0) == -1);
        CHECK(call(SYS_RENAME, U("/RAM Disk/W2/g.txt"), U("/RAM Disk/W/h.txt"), 0) == -1); /* move and rename at once */
        CHECK(call(SYS_RENAME, U("/RAM Disk/W2/g.txt"), U("/RAM Disk/W2/g.txt"), 0) == 0); /* onto itself: nothing to do */
        CHECK(call(SYS_RENAME, U("/RAM Disk"), U("/Disk2"), 0) == -1 && call(SYS_RENAME, U("/"), U("/x"), 0) == -1); /* drives stay */
        long long held = call(SYS_OPEN, U("/RAM Disk/W2/g.txt"), 0, 0);
        CHECK(held >= 3 && call(SYS_RENAME, U("/RAM Disk/W2/g.txt"), U("/RAM Disk/W2/k.txt"), 0) == -1); /* open here */
        call(SYS_CLOSE, (ull)held, 0, 0);
        VFS_READONLY = 1;
        CHECK(call(SYS_RENAME, U("/RAM Disk/W2/g.txt"), U("/RAM Disk/W2/k.txt"), 0) == -1);
        VFS_READONLY = 0;

        /* delete */
        held = call(SYS_OPEN, U("/RAM Disk/W2/g.txt"), 0, 0);
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W2/g.txt"), 0, 0) == -1);            /* still open */
        call(SYS_CLOSE, (ull)held, 0, 0);
        CHECK(call(SYS_RMDIR, U("/RAM Disk/W2"), 0, 0) == -1);                   /* not empty */
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W2"), 0, 0) == -1);                  /* a folder: that is rmdir */
        CHECK(call(SYS_RMDIR, U("/RAM Disk/W2/g.txt"), 0, 0) == -1);             /* a file: that is unlink */
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W2/g.txt"), 0, 0) == 0 && call(SYS_STAT, U("/RAM Disk/W2/g.txt"), U(&e), 0) == -1);
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W2/g.txt"), 0, 0) == -1);            /* gone */
        CHECK(call(SYS_UNLINK, U("/RAM Disk"), 0, 0) == -1 && call(SYS_RMDIR, U("/RAM Disk"), 0, 0) == -1 &&
              call(SYS_RMDIR, U("/"), 0, 0) == -1);                              /* never a drive or the top */
        CHECK(call(SYS_UNLINK, U("nothing"), 0, 0) == -1 && call(SYS_UNLINK, U(""), 0, 0) == -1);
        CHECK(call(SYS_RMDIR, U("/RAM Disk/DIR"), 0, 0) == -1);                  /* (the fake refuses ready-made nodes) */
        CHECK(call(SYS_CHDIR, U("/RAM Disk/W2"), 0, 0) == 0);
        CHECK(call(SYS_RMDIR, U("/RAM Disk/W2"), 0, 0) == -1);                   /* it is the working folder */
        CHECK(call(SYS_CHDIR, U("/"), 0, 0) == 0);
        VFS_READONLY = 1;
        CHECK(call(SYS_RMDIR, U("/RAM Disk/W2"), 0, 0) == -1);
        VFS_READONLY = 0;
        CHECK(call(SYS_RMDIR, U("/RAM Disk/W2"), 0, 0) == 0);
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W/out.txt"), 0, 0) == 0);
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W/out2.txt"), 0, 0) == 0);
        CHECK(call(SYS_UNLINK, U("/RAM Disk/W/big"), 0, 0) == 0);
        CHECK(call(SYS_RMDIR, U("/RAM Disk/W"), 0, 0) == 0);
        CHECK(call(SYS_STAT, U("/RAM Disk/W"), U(&e), 0) == -1);

        /* a kernel thread cannot change anything */
        CURRENT_PROC = 0;
        CHECK(call(SYS_MKDIR, U("/RAM Disk/Z"), 0, 0) == -1 && call(SYS_UNLINK, U("/RAM Disk/readme.txt"), 0, 0) == -1 &&
              call(SYS_RENAME, U("/RAM Disk/readme.txt"), U("/RAM Disk/r"), 0) == -1 && call(SYS_OPEN, U("/RAM Disk/n"), SYS_O_WRITE | SYS_O_CREATE, 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= lock ================= */
    reset();
    VOL_LOCKED = 1;
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 2, 0) == 1);                          /* ask: locked */
    CHECK(call(SYS_LOCK, U("/RAM Disk/DIR"), 0, 0) == 0 && VOL_LOCKED == 0);   /* any path on the drive */
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 2, 0) == 0);
    CHECK(call(SYS_LOCK, U("/RAM Disk/readme.txt"), 1, 0) == 0 && VOL_LOCKED == 1);
    CHECK(call(SYS_CHDIR, U("/RAM Disk"), 0, 0) == 0 && call(SYS_LOCK, U("."), 0, 0) == 0 && VOL_LOCKED == 0); /* the drive I am in */
    CHECK(call(SYS_CHDIR, U("/"), 0, 0) == 0);
    CHECK(call(SYS_LOCK, U("/"), 2, 0) == -1 && call(SYS_LOCK, U("/nothing"), 0, 0) == -1);  /* not a drive */
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 3, 0) == -1 && call(SYS_LOCK, U("/RAM Disk"), ~0ULL, 0) == -1);
    VOL_LOCKABLE = 0;
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 1, 0) == -1 && VOL_LOCKED == 0);      /* cannot be locked: unchanged */
    VOL_LOCKABLE = 1;
    VOL_WRITABLE = 0;
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 0, 0) == -1);                         /* no write driver at all */
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 2, 0) == 0);                          /* asking still works */
    VOL_WRITABLE = 1;
    USER_OK = 0;
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 0, 0) == -1);
    USER_OK = 1;
    CURRENT_PROC = 0;
    CHECK(call(SYS_LOCK, U("/RAM Disk"), 2, 0) == -1);
    CURRENT_PROC = &PROC;

    /* ================= pipes ================= */
    reset();
    {
        int fds[2] = {-1, -1};
        char rb[64];
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == 0 && fds[0] == 3 && fds[1] == 4);       /* the lowest free numbers */
        CHECK(PROC.file[0].flags == 0 && PROC.file[1].flags == PFILE_WRITE && pfile_is_pipe(&PROC.file[0]) && pfile_is_pipe(&PROC.file[1]));
        CHECK(pipe_in_use() == 1);

        /* data in, data out, no waiting */
        BLOCKS = 0;
        CHECK(call(SYS_WRITE, 4, U("hello"), 5) == 5);
        memset(rb, 0, sizeof(rb));
        CHECK(call(SYS_READ, 3, U(rb), 64) == 5 && strcmp(rb, "hello") == 0 && BLOCKS == 0);
        CHECK(call(SYS_WRITE, 4, U("x"), 0) == 0);

        /* an empty pipe with a writer: the reader sleeps until the other side has written */
        BLOCK_ACTION = feed;
        BLOCKS = 0;
        memset(rb, 0, sizeof(rb));
        CHECK(call(SYS_READ, 3, U(rb), 64) == 4 && strcmp(rb, "late") == 0 && BLOCKS == 1);

        /* a full pipe: the writer sleeps until the other side has read; all of it arrives, in order */
        static char big[10000];
        char *out = PIPE_OUT;
        for (int i = 0; i < (int)sizeof(big); i++)
            big[i] = (char)(i * 13 + 5);
        drained = 0;
        BLOCK_ACTION = drain;
        BLOCKS = 0;
        CHECK(call(SYS_WRITE, 4, U(big), 4096) == 4096 && BLOCKS == 0);                 /* exactly fits */
        CHECK(call(SYS_WRITE, 4, U(big + 4096), 4096) == 4096 && BLOCKS >= 1);          /* needs room */
        BLOCK_ACTION = 0;
        while (drained < 8192)
        {
            int r = pipe_read(pfile_pipe(&PROC.file[0]), out + drained, 8192 - drained);
            if (r <= 0)
                break;
            drained += r;
        }
        CHECK(drained == 8192 && memcmp(out, big, 8192) == 0);

        /* the wrong end */
        CHECK(call(SYS_READ, 4, U(rb), 4) == -1 && call(SYS_WRITE, 3, U("x"), 1) == -1);
        CHECK(call(SYS_SEEK, 3, 0, 0) == -1 && call(SYS_SEEK, 4, 0, 2) == -1);        /* a pipe has no position */

        /* every writer gone: the rest is read, then end of input without sleeping */
        CHECK(call(SYS_WRITE, 4, U("tail"), 4) == 4);
        CHECK(call(SYS_CLOSE, 4, 0, 0) == 0);
        BLOCKS = 0;
        CHECK(call(SYS_READ, 3, U(rb), 2) == 2 && call(SYS_READ, 3, U(rb), 64) == 2 && call(SYS_READ, 3, U(rb), 64) == 0);
        CHECK(BLOCKS == 0);
        CHECK(call(SYS_CLOSE, 3, 0, 0) == 0 && pipe_in_use() == 0);                    /* both ends closed: freed */
        CHECK(call(SYS_READ, 3, U(rb), 4) == -1 && call(SYS_CLOSE, 3, 0, 0) == -1);

        /* every reader gone: a write is refused, and a sleeping writer is told */
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == 0);
        CHECK(call(SYS_CLOSE, (ull)fds[0], 0, 0) == 0);
        CHECK(call(SYS_WRITE, (ull)fds[1], U("x"), 1) == -1);
        CHECK(call(SYS_CLOSE, (ull)fds[1], 0, 0) == 0 && pipe_in_use() == 0);
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == 0);
        CHECK(call(SYS_WRITE, (ull)fds[1], U(big), 4096) == 4096);                      /* full */
        BLOCK_ACTION = drop_reader;
        CHECK(call(SYS_WRITE, (ull)fds[1], U("more"), 4) == -1);                        /* slept, then the reader was gone */
        CHECK(call(SYS_CLOSE, (ull)fds[1], 0, 0) == 0 && pipe_in_use() == 0);

        /* a reader sleeping when the last writer ends sees end of input */
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == 0);
        BLOCK_ACTION = drop_writer;
        CHECK(call(SYS_READ, (ull)fds[0], U(rb), 4) == 0);
        CHECK(call(SYS_CLOSE, (ull)fds[0], 0, 0) == 0 && pipe_in_use() == 0);

        /* refusals */
        USER_OK = 0;
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == -1 && pipe_in_use() == 0);
        USER_OK = 1;
        long long files[PROCESS_MAX_FILES];
        int open_n = 0;
        while (open_n < PROCESS_MAX_FILES - 1 && (files[open_n] = call(SYS_OPEN, U("HELLO.ELF"), 0, 0)) >= 3)
            open_n++;
        CHECK(open_n == PROCESS_MAX_FILES - 1 && call(SYS_PIPE, U(fds), 0, 0) == -1 && pipe_in_use() == 0); /* one slot is not enough */
        for (int i = 0; i < open_n; i++)
            call(SYS_CLOSE, (ull)files[i], 0, 0);
        int made = 0;
        while (made < PIPE_MAX + 2 && call(SYS_PIPE, U(fds), 0, 0) == 0)
        {
            made++;
            call(SYS_CLOSE, (ull)fds[1], 0, 0); /* keep only the read ends: a slot per pipe */
            if (made == 4)
                break;
        }
        CHECK(made == 4 && pipe_in_use() == 4);
        for (int i = 0; i < PROCESS_MAX_FILES; i++)
            pfile_close(&PROC.file[i]);
        CHECK(pipe_in_use() == 0);
        CURRENT_PROC = 0;
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == -1);
        CURRENT_PROC = &PROC;

        /* spawn with pipe ends as the child's fd 0 / 1 */
        CHECK(call(SYS_PIPE, U(fds), 0, 0) == 0);
        int io[3] = {-1, fds[1], -1};
        SP_REDIR_N = 0;
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io)) > 0 && SP_REDIR_N == 3);
        CHECK(SP_REDIR[1].node >= PROCESS_PIPE_NODE && (SP_REDIR[1].flags & PFILE_WRITE) && SP_REDIR[0].node < 0);
        PROC.child[0] = 0;
        int in_io[3] = {fds[0], -1, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(in_io)) > 0 && SP_REDIR[0].node >= PROCESS_PIPE_NODE && !(SP_REDIR[0].flags & PFILE_WRITE));
        PROC.child[0] = 0;
        int bad1[3] = {fds[1], -1, -1}, bad2[3] = {-1, fds[0], -1};                      /* the wrong end for the job */
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(bad1)) == -1 && call(SYS_SPAWN, U("HELLO.ELF"), 0, U(bad2)) == -1);
        /* the shell's own stdin / stdout can be a pipe end (a redirected process): reading and writing through fd 0 / 1 */
        PROC.stdio[1] = fds[1] - 3;
        PROC.stdio[0] = fds[0] - 3;
        CHECK(call(SYS_WRITE, 1, U("via fd 1"), 8) == 8 && CON_WRITES == 0);
        memset(rb, 0, sizeof(rb));
        CHECK(call(SYS_READ, 0, U(rb), 64) == 8 && strcmp(rb, "via fd 1") == 0);
        PROC.stdio[0] = PROC.stdio[1] = -1;
        for (int i = 0; i < PROCESS_MAX_FILES; i++)
            pfile_close(&PROC.file[i]);
        CHECK(pipe_in_use() == 0);
    }

    /* ================= signals and ps ================= */
    reset();
    SIG_CALLS = 0;
    SIG_RESULT = 0;
    CHECK(call(SYS_KILL, 42, SIGKILL, 0) == 0 && SIG_PID == 42 && SIG_SIG == SIGKILL && SIG_CALLS == 1);
    CHECK(call(SYS_KILL, 7, SIGINT, 0) == 0 && SIG_SIG == SIGINT && call(SYS_KILL, 7, SIGTERM, 0) == 0 && SIG_SIG == SIGTERM);
    CHECK(call(SYS_KILL, 7, 0, 0) == 0 && SIG_SIG == 0);                            /* only ask */
    SIG_CALLS = 0;
    CHECK(call(SYS_KILL, 0, SIGKILL, 0) == -1 && call(SYS_KILL, (ull)-1, SIGKILL, 0) == -1 && call(SYS_KILL, 0x80000000ULL, SIGKILL, 0) == -1);
    CHECK(call(SYS_KILL, 7, 1, 0) == -1 && call(SYS_KILL, 7, 3, 0) == -1 && call(SYS_KILL, 7, 31, 0) == -1 && call(SYS_KILL, 7, ~0ULL, 0) == -1);
    CHECK(SIG_CALLS == 0);                                                          /* refused before the process layer */
    SIG_RESULT = -1;
    CHECK(call(SYS_KILL, 99, SIGTERM, 0) == -1 && SIG_CALLS == 1);                  /* no such process */
    SIG_RESULT = 0;
    CURRENT_PROC = 0;
    CHECK(call(SYS_KILL, 42, SIGKILL, 0) == -1 && call(SYS_SIGIGNORE, 4, 0, 0) == -1);
    CURRENT_PROC = &PROC;

    PROC.sig_ignore = 0;
    CHECK(call(SYS_SIGIGNORE, 1u << SIGINT, 0, 0) == 0 && PROC.sig_ignore == (1u << SIGINT));
    CHECK(call(SYS_SIGIGNORE, 0xFFFFFFFFULL, 0, 0) == 0 && PROC.sig_ignore == ((1u << SIGINT) | (1u << SIGTERM))); /* SIGKILL is not ignorable */
    CHECK(call(SYS_SIGIGNORE, 1u << SIGKILL, 0, 0) == 0 && PROC.sig_ignore == 0);   /* it replaces the old mask */
    CHECK(call(SYS_SIGIGNORE, 0, 0, 0) == 0 && PROC.sig_ignore == 0);

    {
        SysPsInfo pi;
        memset(&pi, 0x55, sizeof(pi));
        CHECK(call(SYS_PSINFO, 0, U(&pi), 0) == 1 && pi.pid == 10 && pi.ppid == 0 && pi.running == 0 && pi.console == -1 && strcmp(pi.name, "proc0") == 0);
        CHECK(call(SYS_PSINFO, 2, U(&pi), 0) == 1 && pi.pid == 12 && pi.ppid == 2 && pi.running == 0 && pi.console == 1);
        CHECK(call(SYS_PSINFO, 1, U(&pi), 0) == 1 && pi.running == 1 && pi.name[5] == 0 && pi.name[15] == 0);
        CHECK(call(SYS_PSINFO, 3, U(&pi), 0) == 0 && call(SYS_PSINFO, 100000, U(&pi), 0) == 0);
        USER_OK = 0;
        CHECK(call(SYS_PSINFO, 0, U(&pi), 0) == -1);
        USER_OK = 1;
        CURRENT_PROC = 0;
        CHECK(call(SYS_PSINFO, 0, U(&pi), 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= fork, exec, getpid, dup2 ================= */
    reset();
    {
        SyscallFrame fr;
        memset(&fr, 0, sizeof(fr));
        fr.rax = SYS_FORK;
        fr.rcx = 0x400000001234ULL; /* the user RIP SYSCALL saved */
        fr.user_rsp = 0x7FFFFFFFE000ULL;
        FORK_CALLS = 0;
        FORK_RESULT = 5;
        CHECK(syscall_dispatch(&fr) == 5 && FORK_CALLS == 1 && FORK_RIP == 0x400000001234ULL && FORK_RSP == 0x7FFFFFFFE000ULL);
        FORK_RESULT = -1;
        fr.rax = SYS_FORK;
        CHECK(syscall_dispatch(&fr) == -1);                                   /* the process layer refused */
        CURRENT_PROC = 0;
        fr.rax = SYS_FORK;
        CHECK(syscall_dispatch(&fr) == -1 && FORK_CALLS == 2);                /* a kernel thread cannot fork */
        CURRENT_PROC = &PROC;
        FORK_RESULT = 5;

        /* getpid */
        PROC.pid = 33;
        PPID = 77;
        CHECK(call(SYS_GETPID, 0, 0, 0) == 33 && call(SYS_GETPID, 1, 0, 0) == 77 && call(SYS_GETPID, 2, 0, 0) == -1);
        CURRENT_PROC = 0;
        CHECK(call(SYS_GETPID, 0, 0, 0) == -1);
        CURRENT_PROC = &PROC;

        /* exec: the fake returns, so the call fails; what it was asked for is what counts */
        EXEC_CALLS = 0;
        CHECK(call(SYS_EXEC, U("HELLO.ELF"), 0, 0) == -1 && EXEC_CALLS == 1 && EXEC_NODE == 1 && strcmp(EXEC_NAME, "HELLO.ELF") == 0);
        CHECK(EXEC_ARGC == 1 && EXEC_LEN == 10 && strcmp(EXEC_ARGS, "HELLO.ELF") == 0);   /* no argv: the path */
        const char *av[] = {"prog", "-x", "two words", 0};
        CHECK(call(SYS_EXEC, U("hello.elf"), U(av), 0) == -1 && EXEC_CALLS == 2);
        CHECK(EXEC_ARGC == 3 && EXEC_LEN == 5 + 3 + 10 && memcmp(EXEC_ARGS, "prog\0-x\0two words\0", 18) == 0);
        CHECK(call(SYS_EXEC, U("NO-SUCH.ELF"), 0, 0) == -1 && call(SYS_EXEC, U("/RAM Disk/DIR"), 0, 0) == -1 && EXEC_CALLS == 2);
        const char *many[40];
        for (int i = 0; i < 39; i++)
            many[i] = "a";
        many[39] = 0;
        CHECK(call(SYS_EXEC, U("HELLO.ELF"), U(many), 0) == -1 && EXEC_CALLS == 2);      /* too many arguments */
        USER_OK = 0;
        CHECK(call(SYS_EXEC, U("HELLO.ELF"), 0, 0) == -1 && EXEC_CALLS == 2);            /* bad pointer */
        USER_OK = 1;
        CURRENT_PROC = 0;
        CHECK(call(SYS_EXEC, U("HELLO.ELF"), 0, 0) == -1 && EXEC_CALLS == 2);
        CURRENT_PROC = &PROC;

        /* dup2: a fd 0, 1, 2 that is a file or a pipe end, in a slot of its own */
        reset();
        PROC.console = 4;
        long long wfd = call(SYS_OPEN, U("/RAM Disk/dupw.txt"), SYS_O_WRITE | SYS_O_CREATE, 0);
        long long rfd = call(SYS_OPEN, U("/RAM Disk/readme.txt"), 0, 0);
        CHECK(wfd >= 3 && rfd >= 3);
        CHECK(call(SYS_DUP2, (ull)wfd, 1, 0) == 1 && PROC.stdio[1] >= 0 && PROC.stdio[1] != (int)wfd - 3);
        int copy_slot = PROC.stdio[1];
        CHECK(PROC.file[copy_slot].node == PROC.file[wfd - 3].node && (PROC.file[copy_slot].flags & PFILE_WRITE));
        CHECK(call(SYS_CLOSE, (ull)wfd, 0, 0) == 0 && PROC.stdio[1] == copy_slot && PROC.file[copy_slot].node >= 0); /* the copy lives on */
        CON_WRITES = 0;
        CHECK(call(SYS_WRITE, 1, U("via dup2"), 8) == 8 && CON_WRITES == 0);               /* to the file, not the window */
        CHECK(call(SYS_DUP2, (ull)wfd, 1, 0) == -1);                                       /* a closed descriptor */
        CHECK(call(SYS_DUP2, (ull)rfd, 1, 0) == -1 && call(SYS_DUP2, (ull)rfd, 2, 0) == -1);   /* a read-only file as output */
        CHECK(call(SYS_DUP2, (ull)rfd, 0, 0) == 0 && PROC.stdio[0] >= 0);
        CHECK(call(SYS_DUP2, 1, 2, 0) == 2 && PROC.stdio[2] >= 0 && PROC.stdio[2] != PROC.stdio[1]); /* 2>&1 */
        CHECK(call(SYS_DUP2, 2, 2, 0) == 2);                                               /* onto itself */
        CHECK(call(SYS_DUP2, 0, 1, 0) == -1);                                              /* an input as output */
        CHECK(call(SYS_DUP2, 99, 1, 0) == -1 && call(SYS_DUP2, 10, 1, 0) == -1 && call(SYS_DUP2, (ull)-5, 1, 0) == -1);
        CHECK(call(SYS_DUP2, (ull)rfd, 3, 0) == -1 && call(SYS_DUP2, (ull)rfd, 99, 0) == -1);   /* only 0, 1, 2 can be replaced */
        CHECK(call(SYS_DUP2, (ull)-1, 1, 0) == 1 && PROC.stdio[1] == -1);                  /* the console again; the copy's slot is freed */
        CHECK(PROC.file[copy_slot].node == -1);
        CHECK(PROC.stdio[2] >= 0);                                                          /* fd 2 kept its own copy */
        CHECK(call(SYS_WRITE, 1, U("x"), 1) == 1 && CON_WRITES == 1);
        call(SYS_DUP2, (ull)-1, 2, 0);
        call(SYS_DUP2, (ull)-1, 0, 0);
        CHECK(PROC.stdio[0] == -1 && PROC.stdio[1] == -1 && PROC.stdio[2] == -1);
        call(SYS_CLOSE, (ull)rfd, 0, 0);
        int used = 0;
        for (int i = 0; i < PROCESS_MAX_FILES; i++)
            used += PROC.file[i].node >= 0;
        CHECK(used == 0);                                                                   /* nothing left behind */
        CHECK(call(SYS_UNLINK, U("/RAM Disk/dupw.txt"), 0, 0) == 0);

        /* fd 1 and 2 that share one slot (from a spawn redirect) stay open until both let go */
        PROC.file[0].node = 7;
        PROC.file[0].pos = 0;
        PROC.file[0].flags = PFILE_WRITE;
        PROC.stdio[1] = PROC.stdio[2] = 0;
        CHECK(call(SYS_DUP2, (ull)-1, 1, 0) == 1 && PROC.file[0].node == 7 && PROC.stdio[2] == 0);  /* fd 2 still holds it */
        CHECK(call(SYS_DUP2, (ull)-1, 2, 0) == 2 && PROC.file[0].node == -1);
        PROC.stdio[1] = PROC.stdio[2] = -1;

        /* a pipe end through dup2: counted for both, the reader sees the end only when every copy is gone */
        int pf[2];
        CHECK(call(SYS_PIPE, U(pf), 0, 0) == 0);
        int pipe_no = pfile_pipe(&PROC.file[pf[1] - 3]);
        CHECK(call(SYS_DUP2, (ull)pf[1], 1, 0) == 1);
        CHECK(call(SYS_CLOSE, (ull)pf[1], 0, 0) == 0);                                      /* the original write end is closed ... */
        CHECK(call(SYS_WRITE, 1, U("hi"), 2) == 2);                                         /* ... fd 1 still writes into the pipe */
        char got[8];
        CHECK(pipe_read(pipe_no, got, 8) == 2);
        CHECK(pipe_read(pipe_no, got, 8) == PIPE_WOULDBLOCK);                               /* a writer (fd 1) is still there */
        CHECK(call(SYS_DUP2, (ull)-1, 1, 0) == 1);
        CHECK(pipe_read(pipe_no, got, 8) == 0);                                             /* now: end of input */
        CHECK(call(SYS_CLOSE, (ull)pf[0], 0, 0) == 0 && pipe_in_use() == 0);
        /* no free slot: refused, and fd new is unchanged */
        long long held[PROCESS_MAX_FILES];
        int h = 0;
        while (h < PROCESS_MAX_FILES && (held[h] = call(SYS_OPEN, U("HELLO.ELF"), 0, 0)) >= 3)
            h++;
        CHECK(h == PROCESS_MAX_FILES && call(SYS_DUP2, (ull)held[0], 0, 0) == -1 && PROC.stdio[0] == -1);
        for (int i = 0; i < h; i++)
            call(SYS_CLOSE, (ull)held[i], 0, 0);
        CURRENT_PROC = 0;
        CHECK(call(SYS_DUP2, (ull)-1, 1, 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= shared memory ================= */
    reset();
    {
        MAPS_N = 0;
        MAP_CALLS = 0;
        MAP_FAIL_AT = -1;
        const ull B = PROCESS_SHM_BASE;
        long long a = call(SYS_SHM_MAP, U("frame"), 5000, SYS_CREATE);
        CHECK(a == (long long)B && shm_in_use() == 1 && PAGES_OUT == 2);
        CHECK(MAPS_N == 2 && MAPS[0].va == B && MAPS[1].va == B + 4096 && MAPS[0].flags == ASPACE_WRITE && MAPS[0].phys != MAPS[1].phys); /* writable, not executable */
        CHECK(call(SYS_SHM_SIZE, (ull)a, 0, 0) == 8192 && call(SYS_SHM_SIZE, (ull)a + 4096, 0, 0) == -1 && call(SYS_SHM_SIZE, 0, 0, 0) == -1);
        long long b = call(SYS_SHM_MAP, U("other"), 4096, SYS_CREATE);
        CHECK(b == (long long)(B + 3 * 4096));                                  /* after the first one and a page of nothing */
        long long a2 = call(SYS_SHM_MAP, U("frame"), 0, 0);                    /* the same segment again: its own address, the same pages */
        CHECK(a2 > b && MAPS_N == 5 && MAPS[3].phys == MAPS[0].phys && MAPS[4].phys == MAPS[1].phys);
        CHECK(shm_in_use() == 2 && PAGES_OUT == 3);

        /* refused */
        CHECK(call(SYS_SHM_MAP, U("nothing"), 4096, 0) == -1);                  /* no CREATE */
        CHECK(call(SYS_SHM_MAP, U("empty"), 0, SYS_CREATE) == -1);              /* nothing to make */
        CHECK(call(SYS_SHM_MAP, U("huge"), (ull)SHM_MAX_PAGES * 4096 + 1, SYS_CREATE) == -1);
        CHECK(call(SYS_SHM_MAP, U("a/b"), 4096, SYS_CREATE) == -1 && call(SYS_SHM_MAP, U(""), 4096, SYS_CREATE) == -1);
        CHECK(call(SYS_SHM_MAP, U("frame"), 4096, SYS_CREATE | SYS_EXCL) == -1);   /* it exists */
        CHECK(call(SYS_SHM_MAP, U("frame"), 4096, 4) == -1);                    /* no such flag */
        USER_OK = 0;
        CHECK(call(SYS_SHM_MAP, U("frame"), 0, 0) == -1);
        USER_OK = 1;
        CHECK(shm_in_use() == 2 && PAGES_OUT == 3 && MAPS_N == 5);              /* nothing was left behind */

        /* a mapping that fails half way: what was mapped is unmapped, a segment made for it is removed */
        MAP_CALLS = 0;
        MAP_FAIL_AT = 2;
        CHECK(call(SYS_SHM_MAP, U("partial"), 3 * 4096, SYS_CREATE) == -1);
        MAP_FAIL_AT = -1;
        CHECK(shm_in_use() == 2 && PAGES_OUT == 3 && maps_live() == 5);
        MAP_CALLS = 0;
        MAP_FAIL_AT = 1;
        CHECK(call(SYS_SHM_MAP, U("frame"), 0, 0) == -1);                       /* an existing one: it stays */
        MAP_FAIL_AT = -1;
        CHECK(shm_in_use() == 2 && maps_live() == 5);

        /* unmap, unlink, lifetime */
        CHECK(call(SYS_SHM_UNMAP, (ull)b, 0, 0) == 0 && maps_live() == 4);
        CHECK(call(SYS_SHM_UNMAP, (ull)b, 0, 0) == -1 && call(SYS_SHM_UNMAP, (ull)b + 1, 0, 0) == -1 && call(SYS_SHM_UNMAP, 0, 0, 0) == -1);
        CHECK(shm_in_use() == 2);                                               /* "other" is still named */
        CHECK(call(SYS_SHM_UNLINK, U("other"), 0, 0) == 0 && shm_in_use() == 1 && call(SYS_SHM_UNLINK, U("other"), 0, 0) == -1);
        CHECK(call(SYS_SHM_UNLINK, U("frame"), 0, 0) == 0);
        CHECK(call(SYS_SHM_MAP, U("frame"), 0, 0) == -1);                       /* the name is gone ... */
        CHECK(shm_in_use() == 1 && PAGES_OUT == 2 && call(SYS_SHM_SIZE, (ull)a, 0, 0) == 8192); /* ... the mappings are not */
        CHECK(call(SYS_SHM_UNMAP, (ull)a, 0, 0) == 0 && shm_in_use() == 1);     /* one mapping left */
        CHECK(call(SYS_SHM_UNMAP, (ull)a2, 0, 0) == 0 && shm_in_use() == 0 && PAGES_OUT == 0 && maps_live() == 0); /* the last: freed */
        CHECK(PROC.shm_next == 0);                                              /* addresses start over when nothing is mapped */
        CHECK(call(SYS_SHM_MAP, U("again"), 4096, SYS_CREATE) == (long long)B);
        CHECK(call(SYS_SHM_UNLINK, U("again"), 0, 0) == 0);
        process_release_shm(&PROC);                                             /* what a process that ends does */
        CHECK(shm_in_use() == 0 && PAGES_OUT == 0);

        /* only PROCESS_MAX_SHM mappings */
        int made = 0;
        for (int i = 0; i < PROCESS_MAX_SHM + 1; i++)
            if (call(SYS_SHM_MAP, U("same"), 4096, SYS_CREATE) > 0)
                made++;
        CHECK(made == PROCESS_MAX_SHM && shm_in_use() == 1);
        CHECK(call(SYS_SHM_UNLINK, U("same"), 0, 0) == 0);
        process_release_shm(&PROC);
        CHECK(shm_in_use() == 0 && PAGES_OUT == 0);
        for (int i = 0; i < PROCESS_MAX_SHM; i++)
            CHECK(PROC.shm[i].seg == -1);

        CURRENT_PROC = 0;
        CHECK(call(SYS_SHM_MAP, U("k"), 4096, SYS_CREATE) == -1 && call(SYS_SHM_UNMAP, (ull)B, 0, 0) == -1 &&
              call(SYS_SHM_UNLINK, U("k"), 0, 0) == -1 && call(SYS_SHM_SIZE, (ull)B, 0, 0) == -1);
        CURRENT_PROC = &PROC;
        CHECK(shm_in_use() == 0);
    }

    /* ================= GUI windows ================= */
    reset();
    {
        MAPS_N = 0;
        MAP_CALLS = 0;
        MAP_FAIL_AT = -1;
        PROC.pid = 44;
        SysGuiInfo gi;
        memset(&gi, 0, sizeof(gi));
        strcpy(gi.title, "Demo");
        gi.width = 100;
        gi.height = 60;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == 0);
        int gh = gi.handle, x, y, w, h;
        CHECK(gh > 0 && gi.stride == 100 && gi.pixels == PROCESS_SHM_BASE && gui_owner(gh) == 44 && gui_in_use() == 1);
        CHECK(MAPS_N == 6 && MAPS[0].va == PROCESS_SHM_BASE && MAPS[0].flags == ASPACE_WRITE && PROC.shm[0].seg >= 0 && PROC.shm[0].pages == 6);
        CHECK(shm_in_use() == 1 && PAGES_OUT == 6 && strcmp(gui_title(gh), "Demo") == 0);
        CHECK(gui_take_dirty(gh, &x, &y, &w, &h) && w == 100 && h == 60); /* the first picture */

        /* present: the rectangle is packed into two registers, 16 bits each */
        CHECK(call(SYS_GUI_PRESENT, (ull)gh, 10 | (20u << 16), 30 | (40u << 16)) == 0);
        CHECK(gui_take_dirty(gh, &x, &y, &w, &h) && x == 10 && y == 20 && w == 30 && h == 40);
        CHECK(call(SYS_GUI_PRESENT, (ull)gh, 0, 0) == 0 && gui_take_dirty(gh, &x, &y, &w, &h) && w == 100 && h == 60);

        /* a window of someone else, a bad handle: refused */
        CHECK(call(SYS_GUI_PRESENT, 0, 0, 0) == -1 && call(SYS_GUI_PRESENT, (ull)-1, 0, 0) == -1);
        CHECK(call(SYS_GUI_PRESENT, (ull)gh + ((ull)1 << 32), 0, 0) == -1); /* the upper half must not be ignored */
        PROC.pid = 45;
        GuiEvent ev = {0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(call(SYS_GUI_PRESENT, (ull)gh, 0, 0) == -1 && call(SYS_GUI_EVENT, (ull)gh, U(&ev), SYS_GUI_NOWAIT) == -1);
        CHECK(call(SYS_GUI_CTL, (ull)gh, SYS_GUI_DESTROY, 0) == -1 && gui_valid(gh));
        PROC.pid = 44;

        /* events: none yet; a buffer that cannot be written; one that can; waiting for one */
        CHECK(call(SYS_GUI_EVENT, (ull)gh, U(&ev), SYS_GUI_NOWAIT) == 0);
        GuiEvent in = {GUI_EV_KEY, 0, 0, 0, 'q', 'q', 1, KMOD_CTRL};
        CHECK(gui_post(gh, &in) == 1);
        USER_OK = 0;
        CHECK(call(SYS_GUI_EVENT, (ull)gh, U(&ev), SYS_GUI_NOWAIT) == -1);
        USER_OK = 1;
        memset(&ev, 0, sizeof(ev));
        CHECK(call(SYS_GUI_EVENT, (ull)gh, U(&ev), SYS_GUI_NOWAIT) == 1 && ev.type == GUI_EV_KEY && ev.key == 'q' && ev.mods == KMOD_CTRL); /* it was still queued */
        BLOCKS = 0;
        WOKEN_TASKS_N = 0;
        GUI_TEST_HANDLE = gh;
        BLOCK_ACTION = gui_poster;
        CHECK(call(SYS_GUI_EVENT, (ull)gh, U(&ev), 0) == 1 && BLOCKS == 1 && ev.type == GUI_EV_WHEEL && ev.x == 3);
        CHECK(WOKEN_TASKS_N == 1 && WOKEN_TASKS[0] == 7); /* the sleeper (task 7) was the one woken */
        BLOCK_ACTION = raise_int; /* a signal handler is due while it sleeps */
        CHECK(call(SYS_GUI_EVENT, (ull)gh, U(&ev), 0) == -1);
        PROC.sig_pending = 0;
        BLOCK_ACTION = gui_destroyer; /* the window goes while it sleeps */
        CHECK(call(SYS_GUI_EVENT, (ull)gh, U(&ev), 0) == -1 && !gui_valid(gh));
        CHECK(maps_live() == 6 && shm_in_use() == 1 && PAGES_OUT == 6); /* the program's mapping keeps the pixels */
        CHECK(call(SYS_SHM_UNMAP, gi.pixels, 0, 0) == 0 && shm_in_use() == 0 && PAGES_OUT == 0 && maps_live() == 0);

        /* title, destroy */
        memset(&gi, 0, sizeof(gi));
        strcpy(gi.title, "Second");
        gi.width = 64;
        gi.height = 48;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == 0);
        gh = gi.handle;
        CHECK(call(SYS_GUI_CTL, (ull)gh, SYS_GUI_SET_TITLE, U("Renamed")) == 0 && strcmp(gui_title(gh), "Renamed") == 0);
        CHECK(call(SYS_GUI_CTL, (ull)gh, SYS_GUI_SET_TITLE, U("")) == -1 && call(SYS_GUI_CTL, (ull)gh, 2, 0) == -1);
        CHECK(call(SYS_GUI_CTL, (ull)gh, SYS_GUI_DESTROY, 0) == 0 && !gui_valid(gh));
        CHECK(call(SYS_GUI_CTL, (ull)gh, SYS_GUI_DESTROY, 0) == -1 && call(SYS_GUI_PRESENT, (ull)gh, 0, 0) == -1);
        CHECK(call(SYS_SHM_UNMAP, gi.pixels, 0, 0) == 0 && shm_in_use() == 0 && PAGES_OUT == 0);

        /* what cannot be made leaves nothing behind */
        memset(&gi, 0, sizeof(gi));
        strcpy(gi.title, "Bad");
        gi.width = 10;
        gi.height = 60;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1);       /* too narrow */
        gi.width = 100;
        gi.height = GUI_H_MAX + 1;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1);       /* too high */
        gi.height = 60;
        memset(gi.title, 'x', sizeof(gi.title));
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1);       /* no terminating 0 within the 32 bytes */
        memset(gi.title, 0, sizeof(gi.title));
        strcpy(gi.title, "Good");
        USER_OK = 0;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1);
        USER_OK = 1;
        MAP_CALLS = 0;
        MAP_FAIL_AT = 2;                                       /* the mapping fails half way */
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1);
        MAP_FAIL_AT = -1;
        CHECK(gui_in_use() == 0 && shm_in_use() == 0 && PAGES_OUT == 0 && maps_live() == 0);
        for (int i = 0; i < PROCESS_MAX_SHM; i++)              /* no free mapping slot */
            PROC.shm[i].seg = 0;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1 && gui_in_use() == 0 && PAGES_OUT == 0);
        for (int i = 0; i < PROCESS_MAX_SHM; i++)
            PROC.shm[i].seg = -1;

        /* a process that ends takes its windows with it; without a process there are no calls */
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == 0 && gui_in_use() == 1);
        gh = gi.handle;
        CURRENT_PROC = 0;
        CHECK(call(SYS_GUI_CREATE, U(&gi), 0, 0) == -1 && call(SYS_GUI_PRESENT, (ull)gh, 0, 0) == -1 &&
              call(SYS_GUI_EVENT, (ull)gh, U(&ev), SYS_GUI_NOWAIT) == -1 && call(SYS_GUI_CTL, (ull)gh, SYS_GUI_DESTROY, 0) == -1);
        CURRENT_PROC = &PROC;
        gui_release_owner(44);
        process_release_shm(&PROC);
        CHECK(gui_in_use() == 0 && shm_in_use() == 0 && PAGES_OUT == 0);
        PROC.pid = 0;
    }

    /* ================= message queues ================= */
    reset();
    {
        char rb[300];
        long long q = call(SYS_MQ_OPEN, U("jobs"), SYS_CREATE, 0);
        CHECK(q == 3 && PROC.file[0].node == PROCESS_MQ_NODE + 0 && mq_in_use() == 1);
        long long q2 = call(SYS_MQ_OPEN, U("jobs"), 0, 0);                      /* a second descriptor for the same queue */
        CHECK(q2 == 4 && PROC.file[1].node == PROC.file[0].node);

        /* messages keep their boundaries */
        BLOCKS = 0;
        CHECK(call(SYS_WRITE, (ull)q, U("one"), 3) == 3 && call(SYS_WRITE, (ull)q2, U("two!"), 4) == 4);
        memset(rb, 0, sizeof(rb));
        CHECK(call(SYS_READ, (ull)q2, U(rb), 100) == 3 && strcmp(rb, "one") == 0);
        CHECK(call(SYS_READ, (ull)q, U(rb), 100) == 4 && strcmp(rb, "two!") == 0 && BLOCKS == 0);

        /* a buffer that is too small: -1, and the message is still there */
        CHECK(call(SYS_WRITE, (ull)q, U("0123456789"), 10) == 10);
        CHECK(call(SYS_READ, (ull)q, U(rb), 4) == -1);
        CHECK(call(SYS_READ, (ull)q, U(rb), 10) == 10 && memcmp(rb, "0123456789", 10) == 0);
        CHECK(call(SYS_WRITE, (ull)q, U(rb), 0) == 0 && call(SYS_WRITE, (ull)q, U(rb), 257) == -1);   /* an empty write sends nothing; too long is refused */
        static char max[256];
        memset(max, 'm', sizeof(max));
        CHECK(call(SYS_WRITE, (ull)q, U(max), 256) == 256 && call(SYS_READ, (ull)q, U(rb), 300) == 256 && memcmp(rb, max, 256) == 0);

        /* an empty queue: the receiver sleeps until a message comes; a full one: the sender sleeps until there is room */
        BLOCK_ACTION = mq_feed;
        BLOCKS = 0;
        CHECK(call(SYS_READ, (ull)q, U(rb), 300) == 4 && memcmp(rb, "fed!", 4) == 0 && BLOCKS == 1);
        for (int i = 0; i < MQ_DEPTH; i++)
            CHECK(call(SYS_WRITE, (ull)q, U("x"), 1) == 1);
        BLOCK_ACTION = mq_drain;
        BLOCKS = 0;
        CHECK(call(SYS_WRITE, (ull)q, U("last"), 4) == 4 && BLOCKS == 1);
        BLOCK_ACTION = 0;
        int n = 0;
        while (mq_recv(pfile_mq(&PROC.file[0]), rb, sizeof(rb)) > 0)
            n++;
        CHECK(n == MQ_DEPTH);                                                    /* the drain made room for "last": the queue is full again */

        /* what a queue cannot do */
        CHECK(call(SYS_SEEK, (ull)q, 0, 0) == -1);
        CHECK(call(SYS_DUP2, (ull)q, 1, 0) == -1 && call(SYS_DUP2, (ull)q, 0, 0) == -1);   /* not a stdin / stdout */
        int io[3] = {(int)q, -1, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io)) == -1);
        int io2[3] = {-1, (int)q, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io2)) == -1);

        /* closing: a queue lives while a descriptor is open; unlink removes the name */
        CHECK(call(SYS_MQ_UNLINK, U("jobs"), 0, 0) == 0 && call(SYS_MQ_OPEN, U("jobs"), 0, 0) == -1);
        CHECK(mq_in_use() == 1);
        CHECK(call(SYS_WRITE, (ull)q, U("z"), 1) == 1);                          /* still works */
        CHECK(call(SYS_CLOSE, (ull)q, 0, 0) == 0 && mq_in_use() == 1);           /* the other descriptor holds it */
        CHECK(call(SYS_CLOSE, (ull)q2, 0, 0) == 0 && mq_in_use() == 0);
        CHECK(call(SYS_MQ_UNLINK, U("jobs"), 0, 0) == -1);

        /* refused opens */
        CHECK(call(SYS_MQ_OPEN, U("nothing"), 0, 0) == -1);
        CHECK(call(SYS_MQ_OPEN, U("a/b"), SYS_CREATE, 0) == -1 && call(SYS_MQ_OPEN, U(""), SYS_CREATE, 0) == -1);
        CHECK(call(SYS_MQ_OPEN, U("q"), 4, 0) == -1);                            /* no such flag */
        q = call(SYS_MQ_OPEN, U("q"), SYS_CREATE, 0);
        CHECK(q >= 3 && call(SYS_MQ_OPEN, U("q"), SYS_CREATE | SYS_EXCL, 0) == -1);
        USER_OK = 0;
        CHECK(call(SYS_MQ_OPEN, U("q"), 0, 0) == -1);
        USER_OK = 1;
        long long held[PROCESS_MAX_FILES];
        int h = 0;
        while (h < PROCESS_MAX_FILES - 1 && (held[h] = call(SYS_OPEN, U("HELLO.ELF"), 0, 0)) >= 3)
            h++;
        CHECK(h == PROCESS_MAX_FILES - 1 && call(SYS_MQ_OPEN, U("q"), 0, 0) == -1);   /* no free descriptor */
        for (int i = 0; i < h; i++)
            call(SYS_CLOSE, (ull)held[i], 0, 0);
        /* a process that ends gives its queues back (process_release_files) */
        CHECK(mq_in_use() == 1);
        for (int i = 0; i < PROCESS_MAX_FILES; i++)
            pfile_close(&PROC.file[i]);
        CHECK(call(SYS_MQ_UNLINK, U("q"), 0, 0) == 0 && mq_in_use() == 0);
        /* a copy of a descriptor (fork, dup2) counts as a holder */
        q = call(SYS_MQ_OPEN, U("copy"), SYS_CREATE, 0);
        ProcessFile dup = PROC.file[q - 3];
        pfile_ref(&dup);
        CHECK(call(SYS_MQ_UNLINK, U("copy"), 0, 0) == 0 && call(SYS_CLOSE, (ull)q, 0, 0) == 0 && mq_in_use() == 1);
        pfile_close(&dup);
        CHECK(mq_in_use() == 0);
        CURRENT_PROC = 0;
        CHECK(call(SYS_MQ_OPEN, U("k"), SYS_CREATE, 0) == -1 && call(SYS_MQ_UNLINK, U("k"), 0, 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= semaphores ================= */
    reset();
    {
        char rb[8];
        long long s = call(SYS_SEM_OPEN, U("lock"), SYS_CREATE, 2);
        CHECK(s == 3 && PROC.file[0].node == PROCESS_SEM_NODE + 0 && sem_in_use() == 1 && call(SYS_SEM_VALUE, (ull)s, 0, 0) == 2);
        long long s2 = call(SYS_SEM_OPEN, U("lock"), 0, 0);                        /* the same semaphore through a second descriptor */
        CHECK(s2 == 4 && call(SYS_SEM_VALUE, (ull)s2, 0, 0) == 2);

        BLOCKS = 0;
        CHECK(call(SYS_SEM_WAIT, (ull)s, 0, 0) == 0 && call(SYS_SEM_WAIT, (ull)s2, 0, 0) == 0 && BLOCKS == 0);   /* 2 -> 0, no sleeping */
        CHECK(call(SYS_SEM_VALUE, (ull)s, 0, 0) == 0);
        CHECK(call(SYS_SEM_WAIT, (ull)s, 1, 0) == 1 && BLOCKS == 0);              /* only try: it would have to sleep */
        BLOCK_ACTION = sem_poster;
        CHECK(call(SYS_SEM_WAIT, (ull)s, 0, 0) == 0 && BLOCKS == 1);              /* sleeps until another process posts */
        CHECK(call(SYS_SEM_VALUE, (ull)s, 0, 0) == 0);
        CHECK(call(SYS_SEM_POST, (ull)s, 0, 0) == 0 && call(SYS_SEM_POST, (ull)s2, 0, 0) == 0 && call(SYS_SEM_VALUE, (ull)s, 0, 0) == 2);
        CHECK(call(SYS_SEM_WAIT, (ull)s, 1, 0) == 0 && call(SYS_SEM_VALUE, (ull)s, 0, 0) == 1);

        /* what a semaphore descriptor cannot do */
        CHECK(call(SYS_READ, (ull)s, U(rb), 4) == -1 && call(SYS_WRITE, (ull)s, U("x"), 1) == -1 && call(SYS_SEEK, (ull)s, 0, 0) == -1);
        CHECK(call(SYS_DUP2, (ull)s, 1, 0) == -1 && call(SYS_DUP2, (ull)s, 0, 0) == -1);
        int io[3] = {(int)s, -1, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io)) == -1);
        int io2[3] = {-1, (int)s, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io2)) == -1);

        /* the wrong kind of descriptor, and bad arguments */
        long long file = call(SYS_OPEN, U("HELLO.ELF"), 0, 0);
        CHECK(file >= 3 && call(SYS_SEM_WAIT, (ull)file, 1, 0) == -1 && call(SYS_SEM_POST, (ull)file, 0, 0) == -1 && call(SYS_SEM_VALUE, (ull)file, 0, 0) == -1);
        call(SYS_CLOSE, (ull)file, 0, 0);
        CHECK(call(SYS_SEM_WAIT, 0, 1, 0) == -1 && call(SYS_SEM_WAIT, 99, 1, 0) == -1 && call(SYS_SEM_WAIT, (ull)-1, 1, 0) == -1);
        CHECK(call(SYS_SEM_WAIT, 7, 1, 0) == -1);                                  /* a free slot */
        CHECK(call(SYS_SEM_WAIT, (ull)s, 2, 0) == -1);                             /* no such mode */

        /* the largest count */
        long long big = call(SYS_SEM_OPEN, U("big"), SYS_CREATE, 0x7FFFFFFF);
        CHECK(big >= 3 && call(SYS_SEM_POST, (ull)big, 0, 0) == -1 && call(SYS_SEM_VALUE, (ull)big, 0, 0) == 0x7FFFFFFF);
        CHECK(call(SYS_SEM_OPEN, U("huge"), SYS_CREATE, 0x80000000ULL) == -1);
        CHECK(call(SYS_SEM_UNLINK, U("big"), 0, 0) == 0 && call(SYS_CLOSE, (ull)big, 0, 0) == 0 && sem_in_use() == 1);

        /* names, flags, limits */
        CHECK(call(SYS_SEM_OPEN, U("nothing"), 0, 0) == -1 && call(SYS_SEM_OPEN, U("a/b"), SYS_CREATE, 0) == -1 && call(SYS_SEM_OPEN, U(""), SYS_CREATE, 0) == -1);
        CHECK(call(SYS_SEM_OPEN, U("lock"), SYS_CREATE | SYS_EXCL, 0) == -1 && call(SYS_SEM_OPEN, U("lock"), 4, 0) == -1);
        USER_OK = 0;
        CHECK(call(SYS_SEM_OPEN, U("lock"), 0, 0) == -1);
        USER_OK = 1;
        long long held[PROCESS_MAX_FILES];
        int h = 0;
        while (h < PROCESS_MAX_FILES - 2 && (held[h] = call(SYS_OPEN, U("HELLO.ELF"), 0, 0)) >= 3)
            h++;
        CHECK(h == PROCESS_MAX_FILES - 2 && call(SYS_SEM_OPEN, U("lock"), 0, 0) == -1);   /* no free descriptor */
        for (int i = 0; i < h; i++)
            call(SYS_CLOSE, (ull)held[i], 0, 0);

        /* unlink and closing */
        CHECK(call(SYS_SEM_UNLINK, U("lock"), 0, 0) == 0 && call(SYS_SEM_UNLINK, U("lock"), 0, 0) == -1 && call(SYS_SEM_OPEN, U("lock"), 0, 0) == -1);
        CHECK(call(SYS_SEM_POST, (ull)s, 0, 0) == 0);                              /* still works through the descriptors */
        CHECK(call(SYS_CLOSE, (ull)s, 0, 0) == 0 && sem_in_use() == 1);
        CHECK(call(SYS_CLOSE, (ull)s2, 0, 0) == 0 && sem_in_use() == 0);
        CHECK(call(SYS_SEM_POST, (ull)s, 0, 0) == -1);                             /* closed */
        long long c = call(SYS_SEM_OPEN, U("copy"), SYS_CREATE, 1);
        ProcessFile dup = PROC.file[c - 3];
        pfile_ref(&dup);                                                           /* what fork does */
        CHECK(call(SYS_SEM_UNLINK, U("copy"), 0, 0) == 0 && call(SYS_CLOSE, (ull)c, 0, 0) == 0 && sem_in_use() == 1);
        pfile_close(&dup);
        CHECK(sem_in_use() == 0);
        c = call(SYS_SEM_OPEN, U("ends"), SYS_CREATE, 0);
        for (int i = 0; i < PROCESS_MAX_FILES; i++)
            pfile_close(&PROC.file[i]);                                            /* a process that ends */
        CHECK(call(SYS_SEM_UNLINK, U("ends"), 0, 0) == 0 && sem_in_use() == 0);

        CURRENT_PROC = 0;
        CHECK(call(SYS_SEM_OPEN, U("k"), SYS_CREATE, 0) == -1 && call(SYS_SEM_WAIT, 3, 1, 0) == -1 && call(SYS_SEM_UNLINK, U("k"), 0, 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= mutexes and condition variables ================= */
    reset();
    {
        char rb[8];
        long long m = call(SYS_MUTEX_OPEN, U("lk"), SYS_CREATE, 0);
        CHECK(m == 3 && PROC.file[0].node == PROCESS_MUTEX_NODE + 0 && usync_in_use() == 1);
        long long cv = call(SYS_COND_OPEN, U("lk"), SYS_CREATE, 0); /* the same name, the other kind: a different object */
        CHECK(cv == 4 && PROC.file[1].node == PROCESS_COND_NODE + 1 && usync_in_use() == 2);
        long long m2 = call(SYS_MUTEX_OPEN, U("lk"), 0, 0); /* the same mutex through a second descriptor */
        CHECK(m2 == 5 && pfile_usync(&PROC.file[2]) == 0);
        Mutex *mx = usync_mutex(0);
        CHECK(mx && !usync_cond(0) && usync_cond(1) && !usync_mutex(1) && !usync_mutex(9));

        /* lock and unlock */
        BLOCKS = 0;
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 0, 0) == 0 && mx->owner == 7 && BLOCKS == 0);
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 0, 0) == -1 && call(SYS_MUTEX_LOCK, (ull)m2, 1, 0) == -1); /* ours already: no waiting for ourselves */
        CHECK(call(SYS_MUTEX_UNLOCK, (ull)m2, 0, 0) == 0 && mx->owner == -1);
        CHECK(call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == -1); /* not held */
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 1, 0) == 0 && mx->owner == 7 && call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == 0);

        /* held by another process: try says 1, unlock is refused, lock sleeps until it is let go */
        mx->owner = 3;
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 1, 0) == 1 && BLOCKS == 0);
        CHECK(call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == -1 && mx->owner == 3);
        BLOCK_ACTION = mutex_releaser;
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 0, 0) == 0 && BLOCKS == 1 && mx->owner == 7 && mx->waiters == 0);
        CHECK(call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == 0);

        /* a holder that ended does not keep it */
        mx->owner = 3;
        DEAD_TASK = 3;
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 1, 0) == 0 && mx->owner == 7 && BLOCKS == 1);
        CHECK(call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == 0);
        DEAD_TASK = -1;

        /* condition variable: wait lets the mutex go, sleeps until the signal, takes the mutex again */
        CHECK(call(SYS_COND_WAIT, (ull)cv, (ull)m, 0) == -1); /* the mutex is not held */
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 0, 0) == 0);
        CHECK(call(SYS_COND_WAIT, (ull)m, (ull)cv, 0) == -1 && call(SYS_COND_WAIT, (ull)cv, (ull)cv, 0) == -1 && call(SYS_COND_WAIT, (ull)m, (ull)m, 0) == -1);
        CHECK(call(SYS_COND_WAIT, 99, (ull)m, 0) == -1 && call(SYS_COND_WAIT, (ull)cv, 99, 0) == -1);
        BLOCKS = 0;
        SAW_RELEASED = 0;
        BLOCK_ACTION = cond_signaller;
        CHECK(call(SYS_COND_WAIT, (ull)cv, (ull)m, 0) == 0 && BLOCKS == 1 && SAW_RELEASED && mx->owner == 7);
        CHECK(usync_cond(1)->waiters == 0);
        BLOCK_ACTION = cond_broadcaster;
        CHECK(call(SYS_COND_WAIT, (ull)cv, (ull)m, 0) == 0 && BLOCKS == 2 && mx->owner == 7);
        CHECK(call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == 0);

        /* signal and broadcast: nothing to wake is fine, bad arguments are not */
        CHECK(call(SYS_COND_SIGNAL, (ull)cv, 0, 0) == 0 && call(SYS_COND_SIGNAL, (ull)cv, 1, 0) == 0);
        CHECK(call(SYS_COND_SIGNAL, (ull)cv, 2, 0) == -1 && call(SYS_COND_SIGNAL, (ull)m, 0, 0) == -1 && call(SYS_COND_SIGNAL, 99, 0, 0) == -1);

        /* what these descriptors cannot do */
        CHECK(call(SYS_READ, (ull)m, U(rb), 4) == -1 && call(SYS_WRITE, (ull)cv, U("x"), 1) == -1 && call(SYS_SEEK, (ull)m, 0, 0) == -1);
        CHECK(call(SYS_DUP2, (ull)m, 1, 0) == -1 && call(SYS_DUP2, (ull)cv, 0, 0) == -1);
        int io[3] = {(int)m, -1, -1};
        CHECK(call(SYS_SPAWN, U("HELLO.ELF"), 0, U(io)) == -1);
        CHECK(call(SYS_SEM_WAIT, (ull)m, 1, 0) == -1 && call(SYS_SEM_VALUE, (ull)cv, 0, 0) == -1);   /* not semaphores */
        CHECK(call(SYS_MUTEX_LOCK, (ull)cv, 1, 0) == -1 && call(SYS_MUTEX_UNLOCK, (ull)cv, 0, 0) == -1); /* a condition variable is no mutex */
        CHECK(call(SYS_MUTEX_LOCK, 0, 1, 0) == -1 && call(SYS_MUTEX_LOCK, 99, 1, 0) == -1 && call(SYS_MUTEX_LOCK, 7, 1, 0) == -1);
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 2, 0) == -1);

        /* names, flags, limits */
        CHECK(call(SYS_MUTEX_OPEN, U("nothing"), 0, 0) == -1 && call(SYS_MUTEX_OPEN, U("a/b"), SYS_CREATE, 0) == -1 && call(SYS_COND_OPEN, U(""), SYS_CREATE, 0) == -1);
        CHECK(call(SYS_MUTEX_OPEN, U("lk"), SYS_CREATE | SYS_EXCL, 0) == -1 && call(SYS_COND_OPEN, U("lk"), 4, 0) == -1);
        USER_OK = 0;
        CHECK(call(SYS_MUTEX_OPEN, U("lk"), 0, 0) == -1);
        USER_OK = 1;
        CHECK(call(SYS_SYNC_UNLINK, U("lk"), 2, 0) == -1 && call(SYS_SYNC_UNLINK, U("zz"), 0, 0) == -1);

        /* unlink is per kind, the objects live while a descriptor is open */
        CHECK(call(SYS_SYNC_UNLINK, U("lk"), 0, 0) == 0 && call(SYS_SYNC_UNLINK, U("lk"), 0, 0) == -1 && call(SYS_MUTEX_OPEN, U("lk"), 0, 0) == -1);
        CHECK(call(SYS_COND_OPEN, U("lk"), 0, 0) >= 3); /* the condition variable of that name is still there */
        CHECK(call(SYS_MUTEX_LOCK, (ull)m, 1, 0) == 0 && call(SYS_MUTEX_UNLOCK, (ull)m, 0, 0) == 0); /* the unlinked mutex still works */
        for (int i = 0; i < PROCESS_MAX_FILES; i++)
            pfile_close(&PROC.file[i]); /* a process that ends */
        CHECK(usync_in_use() == 1); /* the condition variable's name is still linked */
        CHECK(call(SYS_SYNC_UNLINK, U("lk"), 1, 0) == 0 && usync_in_use() == 0);

        /* the table is full after 16 objects */
        long long fds[PROCESS_MAX_FILES];
        int n = 0;
        char nm[4] = {'a', 0, 0, 0};
        for (int i = 0; i < USYNC_MAX; i++)
        {
            nm[0] = (char)('a' + i);
            long long fd = call(i & 1 ? SYS_COND_OPEN : SYS_MUTEX_OPEN, U(nm), SYS_CREATE, 0);
            CHECK(fd >= 3 || fd == -1);
            if (fd >= 3 && n < PROCESS_MAX_FILES)
                fds[n++] = fd;
            if (fd >= 3)
                call(SYS_CLOSE, (ull)fd, 0, 0); /* closed again: only the names keep them */
        }
        CHECK(usync_in_use() == USYNC_MAX && call(SYS_MUTEX_OPEN, U("zz"), SYS_CREATE, 0) == -1);
        for (int i = 0; i < USYNC_MAX; i++)
        {
            nm[0] = (char)('a' + i);
            CHECK(call(SYS_SYNC_UNLINK, U(nm), i & 1, 0) == 0);
        }
        CHECK(usync_in_use() == 0);

        CURRENT_PROC = 0;
        CHECK(call(SYS_MUTEX_OPEN, U("k"), SYS_CREATE, 0) == -1 && call(SYS_COND_OPEN, U("k"), SYS_CREATE, 0) == -1 &&
              call(SYS_MUTEX_LOCK, 3, 1, 0) == -1 && call(SYS_SYNC_UNLINK, U("k"), 0, 0) == -1);
        CURRENT_PROC = &PROC;
    }

    /* ================= scheduling priority ================= */
    reset();
    {
        PROC.pid = 5;
        PROC.child[0] = 6;
        PRIO_OF[0] = PRIO_OF[1] = 2;
        PRIO_CALLS = 0;
        CHECK(call(SYS_SETPRIO, 0, (ull)-1, 0) == 2 && PRIO_CALLS == 1);                       /* read, the caller itself (pid 0) */
        CHECK(call(SYS_SETPRIO, 0, 4, 0) == 2 && PRIO_OF[0] == 4);                              /* set: gives back the old one */
        CHECK(call(SYS_SETPRIO, 5, 0, 0) == 4 && PRIO_OF[0] == 0);                              /* by its own pid */
        CHECK(call(SYS_SETPRIO, 6, 3, 0) == 2 && PRIO_OF[1] == 3);                              /* a child */
        CHECK(call(SYS_SETPRIO, 6, (ull)-1, 0) == 3 && PRIO_OF[1] == 3);
        CHECK(call(SYS_SETPRIO, 0, 5, 0) == -1 && call(SYS_SETPRIO, 0, 99, 0) == -1 && call(SYS_SETPRIO, 0, ~0ULL - 1, 0) == -1 && PRIO_OF[0] == 0);
        CHECK(call(SYS_SETPRIO, 0x80000000ULL, 1, 0) == -1 && call(SYS_SETPRIO, ~0ULL, 1, 0) == -1);
        int calls = PRIO_CALLS;
        CHECK(call(SYS_SETPRIO, 7, 1, 0) == -1 && call(SYS_SETPRIO, 1, 1, 0) == -1 && PRIO_CALLS == calls); /* not ours: refused before asking */
        PROC.child[0] = 0;                                                                     /* the child was waited for */
        CHECK(call(SYS_SETPRIO, 6, 1, 0) == -1 && PRIO_OF[1] == 3);
        CURRENT_PROC = 0;
        CHECK(call(SYS_SETPRIO, 0, 1, 0) == -1);
        CURRENT_PROC = &PROC;
        PROC.pid = 0;
    }

    /* ================= signal handlers: the system call side ================= */
    reset();
    {
        const ull H = 0x400000003000ULL, E = 0x400000002000ULL;
        PROC.sig_ignore = 1u << SIGINT;
        CHECK(call(SYS_SIGACTION, SIGINT, H, E) == 0 && PROC.sig_handler[SIGINT] == H && PROC.sig_entry == E);
        CHECK(PROC.sig_ignore == 0);                                            /* installing a handler ends "ignore" */
        CHECK(call(SYS_SIGACTION, SIGTERM, H + 16, E) == 0 && PROC.sig_handler[SIGTERM] == H + 16);
        PROC.sig_pending = 1u << SIGINT;
        CHECK(call(SYS_SIGACTION, SIGINT, 0, 0) == 0 && PROC.sig_handler[SIGINT] == 0 && PROC.sig_pending == 0); /* back to the default */
        CHECK(PROC.sig_handler[SIGTERM] == H + 16);
        CHECK(call(SYS_SIGACTION, SIGKILL, H, E) == -1 && call(SYS_SIGACTION, 1, H, E) == -1 && call(SYS_SIGACTION, 40, H, E) == -1 &&
              call(SYS_SIGACTION, 0, H, E) == -1 && call(SYS_SIGACTION, ~0ULL, H, E) == -1);
        CHECK(call(SYS_SIGACTION, SIGINT, 0x1000, E) == -1 && call(SYS_SIGACTION, SIGINT, H, 0x1000) == -1);               /* kernel half */
        CHECK(call(SYS_SIGACTION, SIGINT, 0x800000000000ULL, E) == -1 && call(SYS_SIGACTION, SIGINT, H, ~0ULL) == -1);     /* past the user half */
        CHECK(call(SYS_SIGACTION, SIGINT, 0x3FFFFFFFFFFFULL, E) == -1);                                                     /* just below it */
        CHECK(PROC.sig_handler[SIGINT] == 0);                                                                              /* nothing was changed by the refusals */
        CURRENT_PROC = 0;
        CHECK(call(SYS_SIGACTION, SIGINT, H, E) == -1 && call(SYS_SIGMASK, 4, 0, 0) == -1);
        CURRENT_PROC = &PROC;

        /* the mask */
        PROC.sig_blocked = 0;
        CHECK(call(SYS_SIGMASK, 1u << SIGINT, 0, 0) == 0 && PROC.sig_blocked == (1u << SIGINT));
        CHECK(call(SYS_SIGMASK, 0xFFFFFFFFULL, 0, 0) == (1u << SIGINT) && PROC.sig_blocked == ((1u << SIGINT) | (1u << SIGTERM))); /* only these two can be */
        CHECK(call(SYS_SIGMASK, 1u << SIGKILL, 0, 0) == ((1u << SIGINT) | (1u << SIGTERM)) && PROC.sig_blocked == 0);   /* SIGKILL cannot be blocked */

        /* every system call is followed by a handler that is due, except the one that returns from a handler */
        SIGSYS_CALLS = SIGRET_CALLS = 0;
        SIGDELIV = 0;
        CHECK(call(SYS_YIELD, 0, 0, 0) == 0 && SIGSYS_CALLS == 0);
        SIGDELIV = 1;
        CHECK(call(SYS_YIELD, 0, 0, 0) == 1000 && SIGSYS_CALLS == 1);           /* the frame was changed: the stub's result */
        CHECK(call(SYS_SIGRETURN, 0, 0, 0) == 777 && SIGRET_CALLS == 1 && SIGSYS_CALLS == 1);
        SIGDELIV = 0;

        /* a system call that sleeps gives up when a signal with a handler arrives (and not when it is blocked) */
        PROC.sig_handler[SIGINT] = H;
        PROC.sig_entry = E;
        int pf[2];
        CHECK(call(SYS_PIPE, U(pf), 0, 0) == 0);
        char rb[8];
        BLOCK_ACTION = raise_int;
        BLOCKS = 0;
        CHECK(call(SYS_READ, (ull)pf[0], U(rb), 4) == -1 && BLOCKS == 1);       /* a pipe read */
        PROC.sig_pending = 0;
        PROC.sig_blocked = 1u << SIGINT;
        BLOCKS = 0;
        BLOCK_ACTION = raise_then_feed;                                        /* blocked: the signal does not interrupt; the data does */
        CHECK(call(SYS_READ, (ull)pf[0], U(rb), 4) == 2 && BLOCKS == 2 && PROC.sig_pending == (1u << SIGINT));
        PROC.sig_pending = PROC.sig_blocked = 0;
        BLOCK_ACTION = raise_int;
        static char big[PIPE_SIZE];
        CHECK(call(SYS_WRITE, (ull)pf[1], U(big), 4096) == 4096);               /* fill it */
        PROC.sig_pending = 0;
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_WRITE, (ull)pf[1], U("more"), 4) == -1);                 /* a write that has to wait */
        PROC.sig_pending = 0;
        call(SYS_CLOSE, (ull)pf[0], 0, 0);
        call(SYS_CLOSE, (ull)pf[1], 0, 0);

        long long q = call(SYS_MQ_OPEN, U("sigq"), SYS_CREATE, 0);
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_READ, (ull)q, U(rb), 4) == -1);                          /* a queue receive */
        for (int i = 0; i < MQ_DEPTH; i++)
            call(SYS_WRITE, (ull)q, U("x"), 1);
        PROC.sig_pending = 0;
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_WRITE, (ull)q, U("x"), 1) == -1);                        /* a queue send */
        call(SYS_MQ_UNLINK, U("sigq"), 0, 0);
        call(SYS_CLOSE, (ull)q, 0, 0);
        PROC.sig_pending = 0;

        long long s = call(SYS_SEM_OPEN, U("sigs"), SYS_CREATE, 0);
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_SEM_WAIT, (ull)s, 0, 0) == -1 && call(SYS_SEM_VALUE, (ull)s, 0, 0) == 0);   /* a semaphore wait */
        call(SYS_SEM_UNLINK, U("sigs"), 0, 0);
        call(SYS_CLOSE, (ull)s, 0, 0);
        PROC.sig_pending = 0;

        long long sm = call(SYS_MUTEX_OPEN, U("sigm"), SYS_CREATE, 0), sc = call(SYS_COND_OPEN, U("sigm"), SYS_CREATE, 0);
        Mutex *smx = usync_mutex(pfile_usync(&PROC.file[sm - 3]));
        smx->owner = 3; /* held by someone else: the lock sleeps, a signal ends the wait and nothing stays registered */
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_MUTEX_LOCK, (ull)sm, 0, 0) == -1 && smx->owner == 3 && smx->waiters == 0);
        PROC.sig_pending = 0;
        smx->owner = -1;
        CHECK(call(SYS_MUTEX_LOCK, (ull)sm, 0, 0) == 0);
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_COND_WAIT, (ull)sc, (ull)sm, 0) == -1);                  /* a condition wait: interrupted ... */
        CHECK(smx->owner == 7 && usync_cond(pfile_usync(&PROC.file[sc - 3]))->waiters == 0); /* ... with the mutex held again, nobody registered */
        PROC.sig_pending = 0;
        call(SYS_MUTEX_UNLOCK, (ull)sm, 0, 0);
        call(SYS_SYNC_UNLINK, U("sigm"), 0, 0);
        call(SYS_SYNC_UNLINK, U("sigm"), 1, 0);
        call(SYS_CLOSE, (ull)sm, 0, 0);
        call(SYS_CLOSE, (ull)sc, 0, 0);

        PROC.child[0] = 1234; /* a child that is not finished: wait sleeps ... */
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_WAIT, 1234, 0, 0) == -1 && PROC.child[0] == 1234);       /* ... is interrupted, and the child stays waitable */
        PROC.child[0] = 0;
        PROC.sig_pending = 0;

        CON_LINE = "";                                                         /* the console: nothing typed */
        PROC.console = 4;
        CON_BLOCKS_BEFORE_DATA = 5;
        BLOCK_ACTION = raise_int;
        CHECK(call(SYS_READ, 0, U(rb), 4) == -1);
        BLOCK_ACTION = 0;
        PROC.sig_pending = PROC.sig_blocked = 0;
        PROC.sig_handler[SIGINT] = 0;
        PROC.sig_entry = 0;
    }

    /* ================= yield, exit, unknown ================= */
    reset();
    CHECK(call(SYS_YIELD, 0, 0, 0) == 0 && YIELDS == 1);
    CHECK(call(0, 0, 0, 0) == -1);
    CHECK(call(99, 1, 2, 3) == -1);
    CHECK(call(~0ULL, 0, 0, 0) == -1);
    CHECK(call(SYS_SEEK + 1000, 0, 0, 0) == -1);
    CHECK(YIELDS == 1 && SERIAL_N == 0);

    if (setjmp(EXIT_JMP) == 0)
    {
        call(SYS_EXIT, 42, 0, 0);
        CHECK(!"SYS_EXIT returned");
    }
    CHECK(EXIT_CODE == 42);
    if (setjmp(EXIT_JMP) == 0)
        call(SYS_EXIT, (ull)(long long)-1, 0, 0);
    CHECK(EXIT_CODE == -1);
    if (setjmp(EXIT_JMP) == 0)
        call(SYS_EXIT, 0x1FFFFFFFFULL, 0, 0); /* only the low 32 bits are an int */
    CHECK(EXIT_CODE == -1);

    if (fails == 0)
        printf("syscall tests passed\n");
    return fails ? 1 : 0;
}
