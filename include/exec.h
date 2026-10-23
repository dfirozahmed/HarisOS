/* ============================================================
 * HarisOS exec: run a program that is a file on a drive
 *
 *   int node = exec_find("HELLO.ELF");        a file with that name in the top folder of any drive, or -1
 *   int id = exec_run("HELLO.ELF", &err);     read it, check it, start it as a user process (see process.h)
 *
 * The file is an ELF64 executable (elf.h). exec_run reads all of it into kernel memory, hands it to the ELF loader, which
 * copies the segments into the new process's own pages, and then frees the buffer: the running program does not depend
 * on the file any more. Drives are the ones the VFS shows under "This PC" (FAT32 / exFAT / NTFS volumes of disks and USB sticks).
 *
 * exec_run returns the thread id (>= 0) or a negative EXEC_ERR_*; for EXEC_ERR_LOAD, *err is the ELF_ERR_* reason, or 0 when
 * the cause was lack of memory. Name matching ignores case.
 * ============================================================ */
#ifndef HARIS_EXEC_H
#define HARIS_EXEC_H

#include "process.h"

#define EXEC_MAX_FILE (2 * 1024 * 1024)

#define EXEC_ERR_NOTFOUND -1 /* no such file on any drive */
#define EXEC_ERR_SIZE -2     /* empty, or bigger than EXEC_MAX_FILE */
#define EXEC_ERR_MEMORY -3   /* no memory for the file */
#define EXEC_ERR_READ -4     /* the drive could not read it */
#define EXEC_ERR_LOAD -5     /* the loader refused the file or ran out of memory (see *err) */

int exec_find(const char *name);
int exec_run(const char *name, int *err); /* the program's output goes to the serial port, its input is empty */

/* The same for a file that is already known (a double click in the File Explorer): VFS node `node`, shown as `name`.
 * `console` is the handle of the console window it should write to and read from (console.h), or -1 for none. */
int exec_run_node(int node, const char *name, int console, int *err);

/* The general form: arguments, a parent, the console to share (see ProcessSpawn in process.h). SYS_SPAWN uses it. */
int exec_spawn_node(int node, const char *name, ProcessSpawn *sp, int *err);

/* exec: the running process becomes the program of file `node` (SYS_EXEC); returns only if that failed (EXEC_ERR_*, *err as above). */
int exec_replace_node(int node, const char *name, ProcessSpawn *args, int *err);

/* A sentence for a failed exec_run / exec_run_node: the code it returned and the *err it set. */
const char *exec_error_text(int code, int err);

/* Boot self test: runs HELLO.ELF from the boot drive if there is one, and checks that bad files are refused and nothing
 * leaks. 0 = fine, 1 = there is no HELLO.ELF on any drive (nothing to test), otherwise a negative number. */
int exec_selftest(void);

#endif
