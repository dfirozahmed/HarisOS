/* ============================================================
 * HarisOS system calls: how ring 3 code asks the kernel for something
 *
 *   syscall_init();    once, after gdt_init(): turn on the SYSCALL instruction and point it at our entry code
 *
 * User code puts the call number in rax and up to three arguments in rdi, rsi, rdx, then executes "syscall". The result
 * comes back in rax. Everything except rax, rcx and r11 is preserved (rcx and r11 are used by the instruction itself to
 * save the return address and the flags). The floating point and SSE registers are the process's own and are kept.
 *
 *   SYS_WRITE (1)  rdi = fd (1 or 2: the console), rsi = pointer, rdx = length (at most SYS_WRITE_MAX): print the bytes.
 *                  In the program's console window; with no window, on the serial port. Returns the length, or -1 if
 *                  the fd is not 1 or 2 or the buffer is not (all) readable user memory.
 *   SYS_EXIT  (2)  rdi = exit code: the process ends. Does not return.
 *   SYS_YIELD (3)  give the CPU to another thread. Returns 0.
 *   SYS_READ  (4)  rdi = fd, rsi = pointer, rdx = length: read into the buffer. Returns the number of bytes (0 = end).
 *                  fd 0 is the console: it waits for the user to type a line (and press Enter), then returns that line,
 *                  newline included, at most one line per call; 0 when the window is closed or there is no window.
 *                  A file descriptor from SYS_OPEN reads the file from its current position and moves it; a call may
 *                  return fewer bytes than asked (at most SYS_READ_MAX at once). -1 for a bad fd or buffer.
 *   SYS_OPEN  (5)  rdi = pointer to a NUL-terminated path, rsi = flags (see SYS_O_*; 0 = read only). Returns a file
 *                  descriptor (3 and up) or -1. A path without '/' is a file name looked up in the top folder of every
 *                  drive ("HELLO.ELF"); a path starting with '/' is a VFS path ("/RAM Disk/Documents/readme.txt").
 *                  At most PROCESS_MAX_FILES files are open at once.
 *   SYS_CLOSE (6)  rdi = fd. Returns 0, or -1 if it is not open. (Closing 0, 1 or 2 does nothing.)
 *   SYS_SEEK  (7)  rdi = fd, rsi = offset (signed), rdx = whence (0 = from the start, 1 = from the current position,
 *                  2 = from the end of the file). Returns the new position, or -1 (bad fd, bad whence, negative result).
 *   SYS_BRK   (8)  rdi = new program break (the end of the heap), or 0. The heap starts at the first page after the
 *                  program's last segment and may grow to PROCESS_HEAP_MAX; new pages are zeroed and not executable.
 *                  0 returns the current break. A value outside heap_base ... heap_limit returns -1 and changes
 *                  nothing; otherwise the break becomes rdi and that is returned. Lowering the break does not give
 *                  pages back (they stay mapped, not cleared again, until the process ends). malloc is built on this.
 *   SYS_SPAWN (9)  rdi = path (as for SYS_OPEN), rsi = argv: a NULL-terminated array of string pointers (up to
 *                  PROCESS_ARGS_MAX strings, PROCESS_ARGS_BYTES bytes together), or 0 for just the path as argv[0].
 *                  Starts the program as a child; it shares this process's console. The child begins at _start(argc,
 *                  argv) (rcx, rdx and rdi, rsi). Returns the child's pid (1, 2, ... never reused) or -1. At most
 *                  PROCESS_MAX_CHILDREN children can be outstanding (started and not yet waited for).
 *   SYS_WAIT (10)  rdi = pid, rsi = pointer to an int for the exit code, or 0. Blocks until that child has ended, stores
 *                  its exit code (-1 = ended by a CPU exception) and returns the pid. A child is waited for once. -1 for
 *                  a pid that is not an outstanding child, or a bad pointer (the child stays waitable).
 *   SYS_READDIR (11) rdi = folder path, rsi = index, rdx = pointer to a SysDirEntry. Fills in the index-th entry of the
 *                  folder (folders first, then files, each sorted by name; "/" lists the drives). Returns 1 if it did, 0 when
 *                  index is past the last entry, -1 for a path that is not a folder, a bad pointer or an index >= 512.
 *   SYS_STAT (12)  rdi = path, rsi = pointer to a SysDirEntry: size, is_dir and the name of that file or folder. 0 or -1.
 *   SYS_CHDIR (13) rdi = path: the working folder becomes that folder. 0, or -1 (not a folder). A child starts in its
 *                  parent's working folder.
 *   SYS_GETCWD (14) rdi = buffer, rsi = size: the working folder as an absolute path ("/" = the drives, "/RAM Disk/Documents"),
 *                  NUL-terminated. Returns its length, or -1 if it does not fit.
 *
 * Paths (SYS_OPEN, SYS_SPAWN, SYS_READDIR, SYS_STAT, SYS_CHDIR): one that starts with '/' is absolute. Any other is
 * relative to the working folder; "." and ".." are understood. SYS_OPEN and SYS_SPAWN also accept a plain file name that
 * is not found there and look it up in the top folder of every drive ("HELLO.ELF": where the programs are).
 *   SYS_MKDIR (15) rdi = path of a new folder (inside an existing folder of a drive). 0, or -1.
 *   SYS_UNLINK (16) rdi = path of a file to delete. 0, or -1 (not a file, a drive root, in use by this process).
 *   SYS_RMDIR (17) rdi = path of an EMPTY folder to delete. 0, or -1.
 *   SYS_RENAME (18) rdi = old path, rsi = new path (must not exist): a new name in the same folder, or the same name in
 *                  another folder of the same drive. 0, or -1.
 *   SYS_LOCK (19) rdi = a path on a drive, rsi = 1 lock writing, 0 unlock, 2 ask. Drives start locked (blk.h), so a program that
 *                  writes must unlock the drive first (the "lock" program does). Returns 1 / 0 for "ask" (locked or not), 0 for
 *                  a change, -1 if the path is not on a drive, the drive cannot be written, or the lock cannot be changed.
 *   SYS_PIPE (20) rdi = pointer to int[2]: gets the descriptors of the read end and the write end of a new pipe. 0, or -1.
 *                  SYS_READ on the read end waits until there is data (or every holder of the write end has closed it or ended:
 *                  then it returns 0); SYS_WRITE on the write end waits for room (and returns -1, or the bytes that went in, if
 *                  every holder of the read end is gone). Both ends can be given to children with SYS_SPAWN. A pipe holds 4096
 *                  bytes; it cannot be seeked.
 *   SYS_KILL (21) rdi = pid, rsi = signal (SIGINT, SIGTERM, SIGKILL, or 0 to only ask whether the process exists). The process
 *                  ends at once with exit code 128 + signal, unless it ignores the signal (SIGKILL cannot be ignored). Its pipes and
 *                  files are closed; a parent waiting for it is woken. There are no handlers and no permissions yet: any process
 *                  can signal any process. 0, or -1 (no such process, a signal that does not exist).
 *   SYS_SIGIGNORE (22) rdi = bit mask: bit n set = ignore signal n (only SIGINT and SIGTERM can be). Replaces the old mask.
 *                  A child starts with nothing ignored. The shell and init ignore SIGINT: Ctrl+C ends the program that runs in
 *                  the window, not the shell. Returns 0.
 *   SYS_PSINFO (23) rdi = index, rsi = pointer to a SysPsInfo: the index-th running process (in the order of their threads).
 *                  1, 0 when there is none with that index, -1 for a bad pointer.
 *   SYS_FORK (24) a copy of this process: its own copy of every page, of the open files (a pipe end is counted for both) and of the
 *                  redirections, working folder, console, ignored signals. The parent gets the child's pid (never 0; -1 on failure:
 *                  too many children, no memory); the child starts after the SYSCALL instruction with rax = 0. The child's OTHER
 *                  registers are not the parent's, so call the fork() of the user library (user/libc.c), which saves them on the
 *                  stack (it is copied) and puts them back in both. The child is waited for like a spawned one.
 *   SYS_EXEC (25)  rdi = path, rsi = argv (as for SYS_SPAWN): this process becomes that program (an ELF file) and starts it at its
 *                  entry: same pid, parent, console, folder, open files and redirections (there is no close-on-exec). The old
 *                  program is gone. Does not return when it works; -1, with nothing changed, if there is no such program, it is
 *                  not an ELF program, or memory ran out.
 *   SYS_GETPID (26) rdi = 0: this process's pid; rdi = 1: the parent's (0 = none).
 *   SYS_DUP2 (27)  rdi = old descriptor (from SYS_OPEN / SYS_PIPE; 0..2 = what that one is now; -1 = the console), rsi = new, 0, 1
 *                  or 2: afterwards fd new is the same open file as old (a copy in a slot of its own; closing old keeps new).
 *                  Returns new, or -1. With SYS_FORK and SYS_EXEC this is how a shell redirects: fork, dup2, close, exec.
 *   SYS_SHM_MAP (28) rdi = name (1..31 characters, no '/'), rsi = size in bytes, rdx = flags (SYS_CREATE, SYS_EXCL): shared memory. Finds
 *                  the named segment (or, with CREATE, makes it: rsi bytes rounded up to pages, at most 1 MiB, zeroed) and maps all of it
 *                  readable and writable into this process at an address the kernel chooses (the area from 0x500000000000, with one
 *                  unmapped page after each mapping). Several processes that map the same name see the same bytes at once; a
 *                  forked child keeps the mapping, shared with its parent. Returns the address, or -1. At most 8 mappings.
 *   SYS_SHM_UNMAP (29) rdi = an address SYS_SHM_MAP returned: the pages leave this process.
 *   SYS_SHM_UNLINK (30) rdi = name: the name goes away; the segment lives while anything maps it (and is zero-filled again only by a new
 *                  segment of that name). 0, or -1.
 *   SYS_SHM_SIZE (31) rdi = an address SYS_SHM_MAP returned: the size of the mapping in bytes, or -1.
 *   SYS_MQ_OPEN (32) rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL): a descriptor for a message queue. SYS_WRITE on it sends ONE message
 *                  (1 .. 256 bytes; waits while 16 messages are queued), SYS_READ receives ONE message (waits while there is none; -1,
 *                  and the message stays, if the buffer is smaller than it). Messages keep their boundaries and their order. SYS_CLOSE
 *                  lets go; fork copies the descriptor. At most 8 queues. It cannot be seeked, redirected or given to SYS_SPAWN.
 *   SYS_MQ_UNLINK (33) rdi = name: the name goes away; the queue lives while a descriptor is open. 0, or -1.
 *   SYS_SEM_OPEN (34) rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL), rdx = starting count of a new one: a descriptor for a semaphore (named
 *                  counter, see sem.h), or -1. At most 16. SYS_CLOSE lets go; fork copies it. It cannot be read, written or seeked.
 *   SYS_SEM_WAIT (35) rdi = descriptor, rsi = 0: take one off the count, sleeping while it is 0 (0 returned); rsi = 1: only try (1 if it
 *                  would have to sleep). -1 for something that is not a semaphore.
 *   SYS_SEM_POST (36) rdi = descriptor: the count goes up by one, a sleeper is woken. 0, or -1.
 *   SYS_SEM_VALUE (37) rdi = descriptor: the count now, or -1.   SYS_SEM_UNLINK (38) rdi = name: the name goes away; 0 or -1.
 *   SYS_SIGACTION (39) rdi = signal (SIGINT or SIGTERM), rsi = handler (0 = back to the default action), rdx = the address of the process's
 *                  entry stub (see usersig.h; the user library has it): from now on that signal runs the handler instead of ending the
 *                  process, at the next system call return or timer tick. Both addresses must be in the user half. 0, or -1.
 *   SYS_SIGRETURN (40) only for the stub: back to where the process was when the handler started (see usersig.h).
 *   SYS_SIGMASK (41) rdi = mask (bit n = block signal n; only SIGINT and SIGTERM): a blocked signal waits until it is unblocked. Returns
 *                  the old mask. (A handler runs with its own signal blocked.)
 *   SYS_MUTEX_OPEN (42) rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL): a descriptor for a named mutex (see mutex.h, usync.h), or -1. At most 16
 *                  mutexes and condition variables together. SYS_CLOSE lets go; fork copies it. It cannot be read, written or seeked.
 *   SYS_MUTEX_LOCK (43) rdi = descriptor, rsi = 0: take it, sleeping while another process holds it (0 returned); rsi = 1: only try (1 if it is
 *                  held). -1: not a mutex, the caller holds it already, or a signal handler interrupted the wait. A holder that ends or is
 *                  killed loses it.
 *   SYS_MUTEX_UNLOCK (44) rdi = descriptor: release it and wake the waiter with the highest priority. 0, or -1 if the caller does not hold it.
 *   SYS_COND_OPEN (45) rdi = name, rsi = flags: a descriptor for a named condition variable (a separate namespace from mutexes), or -1.
 *   SYS_COND_WAIT (46) rdi = condition variable, rsi = mutex held by the caller: release the mutex, sleep until signalled, take the mutex again.
 *                  0, or -1 (wrong descriptors, mutex not held, or a signal handler interrupted the sleep: the mutex is held again then too).
 *                  Check the condition in a loop.
 *   SYS_COND_SIGNAL (47) rdi = condition variable, rsi = 0: wake one sleeper, 1: wake all. Lost if nobody sleeps. 0, or -1.
 *   SYS_SYNC_UNLINK (48) rdi = name, rsi = 0 (mutex) or 1 (condition variable): the name goes away; 0 or -1.
 *   A system call that sleeps (read, wait, a semaphore or queue or pipe) returns -1 when a signal with a handler arrives.
 *
 * Ctrl+C: the console window sends SIGINT to every process that writes to that window.
 *
 * Writing: SYS_OPEN with SYS_O_WRITE (and CREATE / TRUNC / APPEND) gives a descriptor SYS_WRITE can use (any length up to
 * SYS_WRITE_MAX; it returns the bytes written, fewer if the drive refused part way, or -1: read only / locked / full
 * drive, the RAM Disk (its files have no content), not opened for writing). A plain name for writing means the working
 * folder, never the programs' folder. Changes reach the drive at once.
 *
 * SYS_SPAWN (9) takes a third argument, rdx: 0, or a pointer to three ints = the caller's open descriptors that become the
 * child's fd 0 (readable file), fd 1 and fd 2 (files opened for writing); -1 leaves that one as it is (the caller's own
 * redirection if it has one, else the console). The caller closes its copies afterwards. This is how a shell does
 * "cmd > file", "cmd >> file" and "cmd < file".
 *   anything else  returns -1.
 *
 * What the CPU does on SYSCALL: RIP goes to LSTAR, CS / SS are loaded from STAR (kernel code / data), the return address
 * is saved in rcx, the flags in r11, and flags are masked with SFMASK (interrupts off). It does NOT change the stack
 * and does NOT save anything else, so the entry code (in syscall.c) first moves to the thread's kernel stack, then
 * saves the registers, and leaves with SYSRET. System calls run with interrupts off, so a call is never preempted and
 * needs no locks; they must therefore be short.
 *
 * The one rule that matters: nothing from ring 3 is trusted. Every pointer is checked (paging_user_range_ok) before the
 * kernel reads or writes through it.
 * ============================================================ */
#ifndef HARIS_SYSCALL_H
#define HARIS_SYSCALL_H

#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_YIELD 3
#define SYS_READ 4
#define SYS_OPEN 5
#define SYS_CLOSE 6
#define SYS_SEEK 7
#define SYS_BRK 8
#define SYS_SPAWN 9
#define SYS_WAIT 10
#define SYS_READDIR 11
#define SYS_STAT 12
#define SYS_CHDIR 13
#define SYS_GETCWD 14
#define SYS_MKDIR 15
#define SYS_UNLINK 16
#define SYS_RMDIR 17
#define SYS_RENAME 18
#define SYS_LOCK 19
#define SYS_PIPE 20
#define SYS_KILL 21
#define SYS_SIGIGNORE 22
#define SYS_PSINFO 23
#define SYS_FORK 24
#define SYS_EXEC 25
#define SYS_GETPID 26
#define SYS_DUP2 27
#define SYS_SHM_MAP 28
#define SYS_SHM_UNMAP 29
#define SYS_SHM_UNLINK 30
#define SYS_SHM_SIZE 31
#define SYS_MQ_OPEN 32
#define SYS_MQ_UNLINK 33
#define SYS_SEM_OPEN 34
#define SYS_SEM_WAIT 35
#define SYS_SEM_POST 36
#define SYS_SEM_VALUE 37
#define SYS_SEM_UNLINK 38
#define SYS_SIGACTION 39
#define SYS_SIGRETURN 40
#define SYS_SIGMASK 41
#define SYS_MUTEX_OPEN 42
#define SYS_MUTEX_LOCK 43
#define SYS_MUTEX_UNLOCK 44
#define SYS_COND_OPEN 45
#define SYS_COND_WAIT 46
#define SYS_COND_SIGNAL 47
#define SYS_SYNC_UNLINK 48

/* flags of SYS_SHM_MAP and SYS_MQ_OPEN */
#define SYS_CREATE 1 /* make it if there is none */
#define SYS_EXCL 2   /* with CREATE: fail if it exists */

/* signals (the numbers of Unix) */
#define SIGINT 2   /* Ctrl+C in the console */
#define SIGKILL 9  /* cannot be ignored */
#define SIGTERM 15

/* SYS_OPEN flags */
#define SYS_O_WRITE 1  /* open for writing (then it cannot be read) */
#define SYS_O_CREATE 2 /* with WRITE: make the file if there is none */
#define SYS_O_TRUNC 4  /* with WRITE: cut it to nothing */
#define SYS_O_APPEND 8 /* with WRITE: every write goes to the end */

#define SYS_WRITE_MAX 4096 /* one write call sends at most this many bytes */
#define SYS_READ_MAX 65536 /* one read call returns at most this many bytes */

/* What SYS_PSINFO gives back (the same layout in user/sys.h). */
typedef struct
{
    int pid;
    int ppid;      /* pid of the parent, 0 = started by the kernel or the parent is gone */
    int running;   /* 1 = ready or running, 0 = waiting (for input, a pipe, a child) */
    int console;   /* -1 = none */
    char name[16];
} SysPsInfo;

/* What SYS_READDIR and SYS_STAT give back (the same layout in user/sys.h). */
typedef struct
{
    unsigned size;   /* bytes, 0 for a folder */
    unsigned is_dir; /* 1 = folder */
    char name[96];   /* NUL-terminated; "" for the root */
} SysDirEntry;

/* What the entry code saves on the kernel stack, lowest address first (the push order in syscall.c). */
typedef struct
{
    unsigned long long r9, r8, r10, rdx, rsi, rdi;
    unsigned long long rax; /* call number in, result out */
    unsigned long long rcx; /* user RIP: where SYSRET goes */
    unsigned long long r11; /* user RFLAGS */
    unsigned long long user_rsp;
} SyscallFrame;

void syscall_init(void);

/* The stack the entry code switches to (and the CPU, for interrupts from ring 3): the top of the running process
 * thread's kernel stack. Set by the scheduler on every switch to a process thread. */
void syscall_set_kernel_stack(unsigned long long top);

/* The work of a system call, called by the entry code with the saved registers. Returns the value for rax. */
long long syscall_dispatch(SyscallFrame *f);

unsigned long long syscall_count(void); /* calls made since boot */

/* For signal.c: after this system call the exit goes through IRETQ to rip / rflags instead of SYSRET (all registers of the frame are
 * then restored as they are, including rcx and r11). */
void syscall_request_iret(unsigned long long rip, unsigned long long rflags);

#endif
