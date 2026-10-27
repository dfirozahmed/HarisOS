/* Shared by the files of the system call layer (kernel/syscall/): what a call needs from ring 3 and from the other groups.
 * Not for use outside this folder; the public interface is include/syscall.h. */
#ifndef HARIS_SYSCALL_INTERNAL_H
#define HARIS_SYSCALL_INTERNAL_H

#include "syscall.h"
#include "aspace.h"
#include "console.h"
#include "exec.h"
#include "gdt.h"
#include "gui.h"
#include "heap.h"
#include "net.h"
#include "mq.h"
#include "pipe.h"
#include "sem.h"
#include "shm.h"
#include "process.h"
#include "rtc.h"
#include "serial.h"
#include "smp.h"
#include "task.h"
#include "timer.h"
#include "uaccess.h"
#include "usersig.h"
#include "vfs.h"

typedef unsigned long long ull;

/* A signal with a handler arrived while this process slept in a system call: the call gives up (-1) so that the handler can run at its
 * return. */
static inline int sig_interrupted(const Process *p) { return p && (p->sig_pending & ~p->sig_blocked & SIG_HANDLER_MASK) != 0; }

/* Ring 3 pointers are copied in pieces of this size through a buffer on the kernel stack. */
#define CHUNK 512

/* ---- sys_util.c ---- */
void sleep_registered(int registered);
void copy_from_user(char *dst, ull src, unsigned n);
void copy_to_user(ull dst, const char *src, unsigned n);
int copy_string_from_user(char *dst, int cap, ull src, const AddressSpace *as);
int resolve_path(const Process *p, const char *in, int bare_ok);

/* ---- sys_fd.c ---- */
long long file_read(Process *p, ull idx, ull ptr, ull len);
int split_parent(const Process *p, const char *path, char *name_out, int name_cap);

/* ---- sys_ipc.c ---- */
ull shm_attach(Process *p, AddressSpace *as, ProcessShm *slot, int s);

/* ---- the calls, dispatched from syscall.c ---- */
long long sys_write(const SyscallFrame *f);
long long sys_read(const SyscallFrame *f);
long long sys_open(const SyscallFrame *f);
long long sys_close(const SyscallFrame *f);
long long sys_seek(const SyscallFrame *f);
long long sys_readdir(const SyscallFrame *f);
long long sys_stat(const SyscallFrame *f);
long long sys_chdir(const SyscallFrame *f);
long long sys_getcwd(const SyscallFrame *f);
long long sys_pipe(const SyscallFrame *f);
long long sys_mkdir(const SyscallFrame *f);
long long sys_unlink(const SyscallFrame *f);
long long sys_rmdir(const SyscallFrame *f);
long long sys_rename(const SyscallFrame *f);
long long sys_lock(const SyscallFrame *f);
long long sys_spawn(const SyscallFrame *f);
long long sys_wait(const SyscallFrame *f);
long long sys_fork(const SyscallFrame *f);
long long sys_exec(const SyscallFrame *f);
long long sys_getpid(const SyscallFrame *f);
long long sys_dup2(const SyscallFrame *f);
long long sys_sigaction(const SyscallFrame *f);
long long sys_sigmask(const SyscallFrame *f);
long long sys_kill(const SyscallFrame *f);
long long sys_setprio(const SyscallFrame *f);
long long sys_sigignore(const SyscallFrame *f);
long long sys_psinfo(const SyscallFrame *f);
long long sys_brk(const SyscallFrame *f);
long long sys_shm_map(const SyscallFrame *f);
long long sys_shm_unmap(const SyscallFrame *f);
long long sys_shm_size(const SyscallFrame *f);
long long sys_shm_unlink(const SyscallFrame *f);
long long sys_mq_open(const SyscallFrame *f);
long long sys_mq_unlink(const SyscallFrame *f);
long long sys_sem_open(const SyscallFrame *f);
long long sys_sem_wait(const SyscallFrame *f);
long long sys_sem_post(const SyscallFrame *f);
long long sys_sem_value(const SyscallFrame *f);
long long sys_sem_unlink(const SyscallFrame *f);
long long sys_mutex_open(const SyscallFrame *f);
long long sys_cond_open(const SyscallFrame *f);
long long sys_mutex_lock(const SyscallFrame *f);
long long sys_mutex_unlock(const SyscallFrame *f);
long long sys_cond_wait(const SyscallFrame *f);
long long sys_cond_signal(const SyscallFrame *f);
long long sys_sync_unlink(const SyscallFrame *f);
long long sys_socket(const SyscallFrame *f);
long long sys_connect(const SyscallFrame *f);
long long sys_resolve(const SyscallFrame *f);
long long sys_wifiscan(const SyscallFrame *f);
long long sys_bind(const SyscallFrame *f);
long long sys_listen(const SyscallFrame *f);
long long sys_accept(const SyscallFrame *f);
long long sys_clock(const SyscallFrame *f);
long long sys_time(const SyscallFrame *f);
long long sys_gui_create(const SyscallFrame *f);
long long sys_gui_present(const SyscallFrame *f);
long long sys_gui_event(const SyscallFrame *f);
long long sys_gui_ctl(const SyscallFrame *f);

#endif
