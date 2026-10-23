/* ============================================================
 * HarisOS pipes: a byte queue between processes
 *
 *   int p = pipe_new();            an empty pipe with nobody on either end yet, or -1 (all PIPE_MAX in use)
 *   pipe_ref(p, 1);                one more holder of the write end (0 = the read end)
 *   pipe_unref(p, 1);              one holder less; the waiting processes on the other side are woken; the pipe is freed
 *                                  when nobody holds either end
 *   n = pipe_write(p, buf, len);   copies what fits (the buffer is PIPE_SIZE bytes), returns the count
 *   n = pipe_read(p, buf, len);    copies what is there, returns the count
 *
 * Pipes never block by themselves (the system call layer does that, see syscall.c): pipe_read returns PIPE_WOULDBLOCK when
 * the pipe is empty and a writer is still around, 0 (end of input) when it is empty and no writer is left. pipe_write returns
 * PIPE_WOULDBLOCK when full, PIPE_BROKEN when no reader is left (nobody will ever read it). A caller that gets
 * PIPE_WOULDBLOCK registers with pipe_wait() and blocks (task_block); the next read / write / unref on the pipe wakes it.
 * Wake-ups can be spurious: the caller checks again.
 *
 * Everything runs with interrupts off (system calls do), so there are no locks.
 * ============================================================ */
#ifndef HARIS_PIPE_H
#define HARIS_PIPE_H

#define PIPE_MAX 16
#define PIPE_SIZE 4096

#define PIPE_WOULDBLOCK -2
#define PIPE_BROKEN -1

int pipe_new(void);
void pipe_ref(int p, int writer);
void pipe_unref(int p, int writer);
int pipe_read(int p, void *buf, unsigned len);
int pipe_write(int p, const void *buf, unsigned len);
void pipe_wait(int p, int writer_side, int task); /* wake `task` at the next change that could unblock a reader (0) / writer (1) */

int pipe_in_use(void);        /* pipes that exist (for the leak check) */
int pipe_valid(int p);        /* is p a pipe that exists */

#endif
