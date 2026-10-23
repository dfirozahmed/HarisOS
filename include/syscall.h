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

/* SYS_OPEN flags */
#define SYS_O_WRITE 1  /* open for writing (then it cannot be read) */
#define SYS_O_CREATE 2 /* with WRITE: make the file if there is none */
#define SYS_O_TRUNC 4  /* with WRITE: cut it to nothing */
#define SYS_O_APPEND 8 /* with WRITE: every write goes to the end */

#define SYS_WRITE_MAX 4096 /* one write call sends at most this many bytes */
#define SYS_READ_MAX 65536 /* one read call returns at most this many bytes */

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

#endif
