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

void task_block(void)
{
    CHECK(host_uaccess == 0);
    BLOCKS++;
    if (CON_BLOCKS_BEFORE_DATA > 0)
        CON_BLOCKS_BEFORE_DATA--; /* "the user pressed Enter while we slept" */
}

/* ---- the file system: four files and a folder ---- */
static VfsNode NODES[6];
static int READ_CALLS;

static unsigned char pattern(int id, unsigned off) { return (unsigned char)(off * 7 + 3 + (unsigned)id); }

static void setup_vfs(void)
{
    memset(NODES, 0, sizeof(NODES));
    strcpy(NODES[1].name, "HELLO.ELF"); NODES[1].used = 1; NODES[1].size = 600;
    strcpy(NODES[2].name, "DIR");       NODES[2].used = 1; NODES[2].is_dir = 1;
    strcpy(NODES[3].name, "readme.txt"); NODES[3].used = 1; NODES[3].size = 20;
    strcpy(NODES[4].name, "BAD.BIN");   NODES[4].used = 1; NODES[4].size = 2000;
    strcpy(NODES[5].name, "EMPTY.BIN"); NODES[5].used = 1; NODES[5].size = 0;
}

const VfsNode *vfs_node(int id) { return id >= 1 && id <= 5 ? &NODES[id] : 0; }

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

int vfs_lookup(const char *path)
{
    if (strcmp(path, "/RAM Disk/readme.txt") == 0)
        return 3;
    if (strcmp(path, "/RAM Disk/DIR") == 0)
        return 2;
    return -1;
}

int vfs_read(int id, unsigned offset, void *buf, unsigned len)
{
    CHECK(host_uaccess == 0);
    READ_CALLS++;
    const VfsNode *n = vfs_node(id);
    if (!n)
        return -1;
    if (id == 4 && offset >= 1024)
        return -1; /* a drive error half way through the file */
    unsigned end = id == 4 ? 1024 : n->size;
    if (offset >= end)
        return 0;
    unsigned got = end - offset < len ? end - offset : len;
    unsigned char *p = (unsigned char *)buf;
    for (unsigned i = 0; i < got; i++)
        p[i] = pattern(id, offset + i);
    return (int)got;
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
    for (int i = 0; i < PROCESS_MAX_FILES; i++)
    {
        PROC.file[i].node = -1;
        PROC.file[i].pos = 0;
    }
    READ_CALLS = 0;
    host_uaccess = 0;
}

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
