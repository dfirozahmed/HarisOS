/* HarisOS exec: see include/exec.h. */
#include "exec.h"
#include "console.h"
#include "elf.h"
#include "heap.h"
#include "paging.h"
#include "pipe.h"
#include "pmm.h"
#include "process.h"
#include "serial.h"
#include "task.h"
#include "vfs.h"

typedef unsigned long long u64;

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b))
            return 0;
    return *a == *b;
}

int exec_find(const char *name)
{
    if (!name || !*name)
        return -1;
    for (int v = 0; v < vfs_volume_count(); v++)
    {
        int dir = vfs_volume_node(v);
        if (dir < 0)
            continue;
        int ids[256];
        int n = vfs_list(dir, ids, 256); /* reads the folder from the drive if that has not happened yet */
        if (n > 256)
            n = 256;
        for (int i = 0; i < n; i++)
        {
            const VfsNode *node = vfs_node(ids[i]);
            if (node && !node->is_dir && same_name(node->name, name))
                return ids[i];
        }
    }
    return -1;
}

/* All of file `node` into a new kernel buffer, or NULL. */
static unsigned char *read_whole(int node, unsigned *size_out, int *err)
{
    const VfsNode *n = vfs_node(node);
    if (!n || n->size == 0 || n->size > EXEC_MAX_FILE)
    {
        *err = EXEC_ERR_SIZE;
        return 0;
    }
    unsigned size = n->size;
    unsigned char *buf = kmalloc(size);
    if (!buf)
    {
        *err = EXEC_ERR_MEMORY;
        return 0;
    }
    unsigned done = 0;
    while (done < size)
    {
        unsigned want = size - done > 65536 ? 65536 : size - done;
        int got = vfs_read(node, done, buf + done, want);
        if (got <= 0)
        {
            kfree(buf);
            *err = EXEC_ERR_READ;
            return 0;
        }
        done += (unsigned)got;
    }
    *size_out = size;
    return buf;
}

int exec_spawn_node(int node, const char *name, ProcessSpawn *sp, int *err)
{
    int e = 0;
    if (err)
        *err = 0;
    unsigned size = 0;
    unsigned char *buf = read_whole(node, &size, &e);
    if (!buf)
        return e;
    int elf_err = 0;
    int id = process_spawn_elf(name, buf, size, sp, &elf_err);
    kfree(buf); /* the process has its own copy of every segment */
    if (id < 0)
    {
        if (err)
            *err = elf_err;
        return EXEC_ERR_LOAD;
    }
    return id;
}

int exec_run_node(int node, const char *name, int console, int *err)
{
    ProcessSpawn sp = {.console = console, .parent = -1};
    return exec_spawn_node(node, name, &sp, err);
}

int exec_run(const char *name, int *err)
{
    if (err)
        *err = 0;
    int node = exec_find(name);
    if (node < 0)
        return EXEC_ERR_NOTFOUND;
    return exec_run_node(node, name, -1, err);
}

const char *exec_error_text(int code, int err)
{
    switch (code)
    {
    case EXEC_ERR_NOTFOUND:
        return "file not found";
    case EXEC_ERR_SIZE:
        return "the file is empty or too big";
    case EXEC_ERR_MEMORY:
        return "not enough memory to read the file";
    case EXEC_ERR_READ:
        return "the drive could not read the file";
    case EXEC_ERR_LOAD:
        return err ? elf_error_text(err) : "not enough memory to start the program";
    default:
        return "unknown error";
    }
}

/* ---- boot self test ----------------------------------------------------------------------------------------- */

static u64 get64(const unsigned char *p)
{
    u64 v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void put64(unsigned char *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

/* ---- the console, end to end ---------------------------------------------------------------------------------------
 * ECHO.ELF reads lines from its console and prints them back. The test plays the user: it starts the program on a console
 * (the same object a window shows, minus the window), waits until the program is blocked in SYS_READ, "types" a line, and
 * looks at what ended up on the screen grid. Then a second run where the window is closed while the program waits. */
static int row_has(int con, const char *needle)
{
    char row[CONSOLE_COLS + 1];
    for (int back = 0; console_row(con, back, row); back++)
    {
        for (int i = 0; row[i]; i++)
        {
            int j = 0;
            while (needle[j] && row[i + j] == needle[j])
                j++;
            if (!needle[j])
                return 1;
        }
    }
    return 0;
}

static void type_line(int con, const char *s)
{
    for (; *s; s++)
        console_key(con, *s);
    console_key(con, '\n');
}

/* give the program the CPU until it is waiting for input (blocked), or has ended */
static int wait_blocked(int id)
{
    for (int i = 0; i < 400; i++)
    {
        TaskState s = task_state(id);
        if (s == TASK_BLOCKED)
            return 1;
        if (s == TASK_DEAD || s == TASK_UNUSED)
            return 0;
        task_yield();
    }
    return 0;
}

static int echo_test(int node)
{
    ProcessStats s0, s1;
    process_stats(&s0);

    int con = console_open("selftest");
    if (con < 0)
        return -20;
    int err = 0;
    int id = exec_run_node(node, "ECHO.ELF", con, &err);
    if (id < 0)
    {
        console_close(con);
        return -21;
    }
    if (!wait_blocked(id)) /* it prints its prompt, then waits in SYS_READ: blocked, not spinning */
        return -22;
    if (!row_has(con, "type a line") || !row_has(con, ">"))
        return -23;

    type_line(con, "hi"); /* Enter wakes the program: it prints the line back and waits again */
    if (!wait_blocked(id))
        return -24;
    if (!row_has(con, "you said: hi (line 1, 2 characters)"))
        return -25;

    type_line(con, "quit");
    if (process_wait(id) != 0)
        return -26;
    process_stats(&s1);
    if (s1.exited != s0.exited + 1 || s1.last_exit != 2) /* it read two lines */
        return -27;
    if (!row_has(con, "bye") || !row_has(con, "[process exited, code 2]"))
        return -28;
    if (!console_finished(con))
        return -29;
    console_close(con);

    /* the window is closed while the program waits for a line: it is woken, reads end of input, and finishes */
    con = console_open("selftest 2");
    if (con < 0)
        return -30;
    id = exec_run_node(node, "ECHO.ELF", con, &err);
    if (id < 0)
    {
        console_close(con);
        return -31;
    }
    if (!wait_blocked(id))
        return -32;
    console_close(con);
    if (process_wait(id) != 0)
        return -33;
    process_stats(&s0);
    if (s0.last_exit != 0 || s0.exited != s1.exited + 1)
        return -34;
    return 0;
}

/* ---- the shell, end to end ------------------------------------------------------------------------------------------------
 * SH.ELF on a console, driven like a user at the keyboard. The shell is BLOCKED both when it waits for a line and when it waits
 * for a program it started, so "ready" means: blocked and the bottom row is a bare prompt (it ends with '>'). */
static int wait_prompt(int con, int id)
{
    char row[CONSOLE_COLS + 1];
    for (int i = 0; i < 3000; i++)
    {
        TaskState s = task_state(id);
        if (s == TASK_DEAD || s == TASK_UNUSED)
            return 0;
        if (s == TASK_BLOCKED && console_row(con, 0, row))
        {
            int n = 0;
            while (row[n])
                n++;
            if (n > 0 && row[n - 1] == '>')
                return 1;
        }
        task_yield();
    }
    return 0;
}

static int shell_test(int node)
{
    int con = console_open("shell test");
    if (con < 0)
        return -270;
    int err = 0;
    int id = exec_run_node(node, "SH.ELF", con, &err);
    if (id < 0)
    {
        console_close(con);
        return -271;
    }
    static const struct
    {
        const char *line, *expect;
    } STEPS[] = {
        {"", "HarisOS shell"},
        {"echo hello    world", "hello world"},
        {"cd \"/RAM Disk\"", "/RAM Disk>"},
        {"ls", "Documents/"},
        {"ls -l Documents", "612  notes.txt"},
        {"cat readme.txt", "> cat readme.txt"}, /* a RAM Disk file has no bytes: nothing printed, no error either */
        {"cat nothing.txt", "cat: cannot open 'nothing.txt'"},
        {"nosuchcmd", "cannot run 'nosuchcmd'"},
        {"cd nowhere", "cd: no such folder: nowhere"},
        {"hello", "[exit 42]"},
        {"crash", "[crashed]"},
        {"cd ..", "/>"},
    };
    int result = 0;
    for (unsigned i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]) && result == 0; i++)
    {
        if (i > 0)
            type_line(con, STEPS[i].line);
        if (!wait_prompt(con, id))
            result = -272 - (int)i;
        else if (!row_has(con, STEPS[i].expect))
            result = -290 - (int)i;
    }
    if (result == 0)
    {
        type_line(con, "exit 7");
        ProcessStats s;
        if (process_wait(id) != 0)
            result = -310;
        else
        {
            process_stats(&s);
            if (s.last_exit != 7)
                result = -311;
        }
    }
    else
    {
        console_close(con); /* wakes a blocked shell: it sees end of input and ends */
        process_wait(id);
        return result;
    }
    console_close(con);
    return result;
}

/* ---- init, end to end ---------------------------------------------------------------------------------------------------
 * INIT.ELF runs the shell on a console and starts it again when it ends; closing the window ends both. */
static int init_test(int node)
{
    int con = console_open("init test");
    if (con < 0)
        return -320;
    int err = 0;
    int id = exec_run_node(node, "INIT.ELF", con, &err);
    if (id < 0)
    {
        console_close(con);
        return -321;
    }
    int result = 0;
    if (!wait_prompt(con, id))
        result = -322;
    else if (!row_has(con, "init: HarisOS is up") || !row_has(con, "HarisOS shell"))
        result = -323;
    else
    {
        type_line(con, "exit 3"); /* the shell ends; init says so and starts a new one */
        if (!wait_prompt(con, id))
            result = -324;
        else if (!row_has(con, "init: the shell ended (3), starting it again"))
            result = -325;
        else
        {
            type_line(con, "echo second shell"); /* the new shell works */
            if (!wait_prompt(con, id))
                result = -326;
            else if (!row_has(con, "second shell"))
                result = -327;
        }
    }
    console_close(con); /* the shell reads end of input, exits with 130, init ends too */
    if (process_wait(id) != 0)
        return result ? result : -328;
    if (result == 0 && process_exit_code(id) != 0)
        result = -329;
    return result;
}

/* ---- floating point state per process ----------------------------------------------------------------------------------
 * FPUA.ELF and FPUB.ELF fill xmm0-15, the x87 stack, MXCSR and the x87 control word with values of their own (a different
 * rounding mode each), spin, and check that nothing changed. They run at the same time, so the timer interrupt moves the CPU
 * between them many times. Each exits with 44 if its registers were always its own. */
static int fpu_test(void)
{
    unsigned long long switches0 = task_switches();
    int err = 0;
    int a = exec_run("FPUA.ELF", &err);
    int b = exec_run("FPUB.ELF", &err);
    if (a < 0 || b < 0)
    {
        if (a >= 0)
            process_wait(a);
        if (b >= 0)
            process_wait(b);
        return -40;
    }
    if (process_wait(a) != 0 || process_wait(b) != 0)
        return -41;
    int ca = process_exit_code(a), cb = process_exit_code(b);
    if (ca != 44)
        return -42 - (ca > 0 && ca < 10 ? ca : 0); /* -43 .. -46: which kind of register changed in A */
    if (cb != 44)
        return -47 - (cb > 0 && cb < 10 ? cb : 0); /* -48 .. -51 in B */
    serial_puts("HARISOS: fpu test: ");
    serial_putd((long long)(task_switches() - switches0));
    serial_puts(" thread switches while the two programs ran\n");
    return 0;
}

int exec_selftest(void)
{
    int node = exec_find("HELLO.ELF");
    if (node < 0)
        return 1;

    /* "free memory" = free pages + pages the heap holds as arenas (the heap may keep one it grew for the file buffer) */
    HeapStats h0;
    heap_stats(&h0);
    u64 free0 = pmm_free_pages() + h0.arena_bytes / 4096;
    unsigned tables0 = paging_tables(); /* page tables made for stack guard pages stay (see below) */
    u64 tag0[PMM_TAG_COUNT]; /* who owns the pages, to say who kept one if the count is off at the end */
    for (int t = 0; t < PMM_TAG_COUNT; t++)
        tag0[t] = pmm_tag_pages((PmmTag)t);
    ProcessStats s0;
    process_stats(&s0);

    /* the real program: its output goes to the serial port, and it exits with 42 only if .data, .bss, .rodata and its
     * stack all had the right contents */
    int err = 0;
    int id = exec_run("hello.elf", &err); /* (case does not matter) */
    if (id < 0)
        return -1;
    if (process_wait(id) != 0)
        return -2;
    ProcessStats s1;
    process_stats(&s1);
    if (s1.exited != s0.exited + 1 || s1.last_exit != 42)
        return -3;

    /* the file system calls: FILES.ELF opens, reads, seeks and closes files and checks that bad arguments are refused; it
     * exits with 43 if all of its (many) checks held, else with the number of the first that did not */
    id = exec_run("FILES.ELF", &err);
    if (id < 0)
        return -10;
    if (process_wait(id) != 0)
        return -11;
    ProcessStats s2;
    process_stats(&s2);
    if (s2.exited != s1.exited + 1 || s2.last_exit != 43)
        return -12 - (s2.last_exit > 0 && s2.last_exit < 43 ? s2.last_exit : 0); /* -13 ... -54: which check failed */

    /* the user C library: LIBCTEST.ELF checks string / memory / number / printf functions (optional); exits with 44 */
    if (exec_find("LIBCTEST.ELF") >= 0)
    {
        id = exec_run("LIBCTEST.ELF", &err);
        if (id < 0)
            return -60;
        if (process_wait(id) != 0)
            return -61;
        ProcessStats s3;
        process_stats(&s3);
        if (s3.last_exit != 44)
            return -62 - (s3.last_exit > 0 && s3.last_exit < 44 ? s3.last_exit : 0); /* -63 ... -105: which check failed */
    }

    /* SYS_BRK and the heap (malloc / free / realloc): HEAPTEST.ELF exits with 45 if all checks held (optional) */
    if (exec_find("HEAPTEST.ELF") >= 0)
    {
        id = exec_run("HEAPTEST.ELF", &err);
        if (id < 0)
            return -110;
        if (process_wait(id) != 0)
            return -111;
        ProcessStats s4;
        process_stats(&s4);
        if (s4.last_exit != 45)
            return -112 - (s4.last_exit > 0 && s4.last_exit < 45 ? s4.last_exit : 0); /* -113 ... -156 */
    }

    /* SYS_SPAWN / SYS_WAIT and program arguments: PROCTEST.ELF starts HELLO, ARGTEST and CRASH as children (optional,
     * needs all four files); exits with 46 if all checks held */
    if (exec_find("PROCTEST.ELF") >= 0 && exec_find("ARGTEST.ELF") >= 0 && exec_find("CRASH.ELF") >= 0)
    {
        id = exec_run("PROCTEST.ELF", &err);
        if (id < 0)
            return -160;
        if (process_wait(id) != 0)
            return -161;
        ProcessStats s5;
        process_stats(&s5);
        if (s5.last_exit != 46) /* the parent ends last, so its code is the last one */
            return -162 - (s5.last_exit > 0 && s5.last_exit < 46 ? s5.last_exit : 0); /* -163 ... -207 */
    }

    /* folders and paths: DIRTEST.ELF checks SYS_READDIR / STAT / CHDIR / GETCWD and relative paths (optional); exits with 47 */
    if (exec_find("DIRTEST.ELF") >= 0)
    {
        id = exec_run("DIRTEST.ELF", &err);
        if (id < 0)
            return -210;
        if (process_wait(id) != 0)
            return -211;
        ProcessStats s6;
        process_stats(&s6);
        if (s6.last_exit != 47)
            return -212 - (s6.last_exit > 0 && s6.last_exit < 47 ? s6.last_exit : 0); /* -213 ... -258 */
    }

    /* the shell: SH.ELF starts LS / HELLO / CRASH on a console (optional, needs those files) */
    int sh_node = exec_find("SH.ELF");
    if (sh_node >= 0 && exec_find("LS.ELF") >= 0 && exec_find("CAT.ELF") >= 0 && exec_find("CRASH.ELF") >= 0)
    {
        int sh = shell_test(sh_node);
        if (sh != 0)
            return sh;
    }

    /* pipes: PIPETEST.ELF connects itself to UPPER.ELF through two pipes (optional); exits with 48 */
    if (exec_find("PIPETEST.ELF") >= 0 && exec_find("UPPER.ELF") >= 0)
    {
        id = exec_run("PIPETEST.ELF", &err);
        if (id < 0)
            return -330;
        if (process_wait(id) != 0)
            return -331;
        ProcessStats s7;
        process_stats(&s7);
        if (s7.last_exit != 48)
            return -332 - (s7.last_exit > 0 && s7.last_exit < 48 ? s7.last_exit : 0); /* -333 ... -379 */
        if (pipe_in_use() != 0)
            return -380; /* a pipe was not given back */
    }

    /* init: INIT.ELF keeps the shell running (optional, needs the shell's files) */
    int init_node = exec_find("INIT.ELF");
    if (init_node >= 0 && sh_node >= 0 && exec_find("LS.ELF") >= 0)
    {
        int it = init_test(init_node);
        if (it != 0)
            return it;
    }

    /* floating point / SSE registers are kept apart per process (FPUA.ELF and FPUB.ELF are optional) */
    if (exec_find("FPUA.ELF") >= 0 && exec_find("FPUB.ELF") >= 0)
    {
        int ft = fpu_test();
        if (ft != 0)
            return ft;
    }

    /* the console: input, output, blocking, waking, and what happens when the window goes (ECHO.ELF is optional) */
    int echo_node = exec_find("ECHO.ELF");
    if (echo_node >= 0)
    {
        int et = echo_test(echo_node);
        if (et != 0)
            return et;
    }

    /* files the loader must refuse, made by damaging a copy of the real one */
    unsigned size = 0;
    int rerr = 0;
    unsigned char *img = read_whole(node, &size, &rerr);
    if (!img)
        return -4;
    int result = 0;
    u64 phoff = size >= 64 ? get64(img + 32) : 0;
    if (size < 64 + 56 || phoff > size - 56)
        result = -5; /* not what a linker makes */
    else
    {
        unsigned char saved[8];
        int e1 = 0, e2 = 0, e3 = 0;

        img[0] = 'X'; /* no longer an ELF file */
        int a = process_start_elf("BAD1", img, size, -1, &e1);
        img[0] = 0x7F;

        for (int i = 0; i < 8; i++)
            saved[i] = img[phoff + 16 + i];
        put64(img + phoff + 16, 0x1000); /* the first segment would land in kernel memory */
        int b = process_start_elf("BAD2", img, size, -1, &e2);
        for (int i = 0; i < 8; i++)
            img[phoff + 16 + i] = saved[i];

        int c = process_start_elf("BAD3", img, size / 2, -1, &e3); /* cut in half */

        if (a != -1 || e1 != ELF_ERR_FORMAT)
            result = -6;
        else if (b != -1 || e2 != ELF_ERR_SEGMENT)
            result = -7;
        else if (c != -1 || e3 == ELF_OK)
            result = -8;
    }
    kfree(img);
    if (result)
        return result;

    HeapStats h1;
    heap_stats(&h1);
    /* A thread's stack has an unmapped guard page (task.c). Unmapping it splits a 2 MiB page of the kernel's identity map, which
     * takes one page table; the table is kept for good (nothing maps the 2 MiB page back together). Which stacks land in a not yet
     * split region depends on where the allocator hands pages out, so programs that run many threads at once can add tables
     * (proctest does). Those pages are not leaks: they are counted by paging_tables() and subtracted here. */
    unsigned new_tables = paging_tables() - tables0;
    if (pmm_free_pages() + h1.arena_bytes / 4096 + new_tables != free0)
    {
        serial_puts("HARISOS: exec self test: free pages before ");
        serial_putd((long long)free0);
        serial_puts(", after (with the new page tables) ");
        serial_putd((long long)(pmm_free_pages() + h1.arena_bytes / 4096 + new_tables));
        serial_puts("\n");
        for (int t = 0; t < PMM_TAG_COUNT; t++)
            if (pmm_tag_pages((PmmTag)t) != tag0[t])
            {
                serial_puts("HARISOS: exec self test: pages of tag ");
                serial_putd(t);
                serial_puts(" went from ");
                serial_putd((long long)tag0[t]);
                serial_puts(" to ");
                serial_putd((long long)pmm_tag_pages((PmmTag)t));
                serial_puts("\n");
            }
        return -9; /* a page, a stack or a table was not given back */
    }
    return 0;
}
