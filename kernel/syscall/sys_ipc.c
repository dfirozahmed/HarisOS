/* System calls: shared memory, message queues, semaphores, mutexes and condition variables. */
#include "syscall_internal.h"

/* ---- shared memory and message queues ---------------------------------------------------------------------------------------- */
static ProcessShm *shm_slot_of(Process *p, ull addr)
{
    for (int i = 0; i < p->shm_cap; i++)
        if (p->shm[i].seg >= 0 && p->shm[i].addr == addr)
            return &p->shm[i];
    return 0;
}

/* Map all of segment s, readable and writable, into p's space at the next free address and record it in `slot` (a free entry of
 * p->shm). Returns the address, or 0 with nothing mapped. (SYS_SHM_MAP, and SYS_GUI_CREATE for the pixels of a window.) */
ull shm_attach(Process *p, AddressSpace *as, ProcessShm *slot, int s)
{
    unsigned pages = shm_pages(s);
    ull addr = p->shm_next ? p->shm_next : PROCESS_SHM_BASE;
    int ok = addr + (ull)pages * 4096 <= PROCESS_SHM_END;
    unsigned done = 0;
    for (; ok && done < pages; done++)
        if (aspace_map_shared(as, addr + (ull)done * 4096, shm_page(s, done), ASPACE_WRITE) != 0)
            ok = 0;
    if (!ok)
    {
        for (unsigned i = 0; i < done; i++)
            aspace_unmap_shared(as, addr + (ull)i * 4096);
        return 0;
    }
    shm_ref(s);
    slot->seg = s;
    slot->addr = addr;
    slot->pages = pages;
    p->shm_next = addr + (ull)(pages + 1) * 4096; /* one page of nothing after it, so a run past the end faults */
    return addr;
}

/* SYS_SHM_MAP: rdi = name, rsi = size in bytes (for a segment that is made: rounded up to pages, at most SHM_MAX_PAGES), rdx = flags
 * (SHM_CREATE, SHM_EXCL). Finds or makes the segment and maps all of it, readable and writable, into this process at an address the
 * kernel picks. Returns the address, or -1. A segment made here starts as zeros. */
long long sys_shm_map(const SyscallFrame *f)
{
    ull name_ptr = f->rdi, size = f->rsi, flags = f->rdx;
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (flags & ~(ull)(SHM_CREATE | SHM_EXCL)))
        return -1;
    char name[SHM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), name_ptr, as) < 0 || !shm_valid_name(name))
        return -1;
    ProcessShm *slot = process_shm_slot(p); /* a free entry; the list grows when all are in use */
    if (!slot)
        return -1;
    int existed = shm_open(name, 0, 0) >= 0;
    int s = shm_open(name, size, (unsigned)flags);
    if (s < 0)
        return -1;
    ull addr = shm_attach(p, as, slot, s);
    if (!addr)
    {
        if (!existed)
            shm_unlink(name); /* made just now and not usable: do not leave it behind */
        return -1;
    }
    return (long long)addr;
}

/* SYS_SHM_UNMAP: rdi = an address SYS_SHM_MAP returned. The pages leave this process (the segment lives on for others and by name). */
long long sys_shm_unmap(const SyscallFrame *f)
{
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    ProcessShm *slot = shm_slot_of(p, f->rdi);
    if (!slot)
        return -1;
    for (unsigned i = 0; i < slot->pages; i++)
        aspace_unmap_shared(as, slot->addr + (ull)i * 4096);
    shm_unref(slot->seg);
    slot->seg = -1;
    int any = 0;
    for (int i = 0; i < p->shm_cap; i++)
        any |= p->shm[i].seg >= 0;
    if (!any)
        p->shm_next = 0;
    return 0;
}

/* SYS_SHM_SIZE: rdi = an address SYS_SHM_MAP returned: the size of the mapping in bytes (a whole number of pages), or -1. */
long long sys_shm_size(const SyscallFrame *f)
{
    Process *p = process_current();
    ProcessShm *slot = p ? shm_slot_of(p, f->rdi) : 0;
    return slot ? (long long)slot->pages * 4096 : -1;
}

/* SYS_SHM_UNLINK: rdi = name. The name goes away; the pages stay until nobody maps them. 0 or -1. */
long long sys_shm_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[SHM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !shm_valid_name(name))
        return -1;
    return shm_unlink(name);
}

/* SYS_MQ_OPEN: rdi = name, rsi = flags (MQ_CREATE, MQ_EXCL). A descriptor for the queue: SYS_WRITE sends one message, SYS_READ
 * receives one (see mq.h); SYS_CLOSE lets go. -1: no such queue, bad name, no free descriptor or queue. */
long long sys_mq_open(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (f->rsi & ~(ull)(MQ_CREATE | MQ_EXCL)))
        return -1;
    char name[MQ_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !mq_valid_name(name))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int q = mq_open(name, (unsigned)f->rsi);
    if (q < 0)
        return -1;
    p->file[slot].node = PROCESS_MQ_NODE + q;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    mq_ref(q);
    return PROCESS_FD_FIRST + slot;
}

long long sys_mq_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[MQ_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !mq_valid_name(name))
        return -1;
    return mq_unlink(name);
}

/* ---- semaphores -------------------------------------------------------------------------------------------------------------- */
/* SYS_SEM_OPEN: rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL), rdx = the starting count for one that is made (0 .. 2^31-1).
 * A descriptor for the semaphore, or -1. SYS_CLOSE lets go; fork copies the descriptor. */
long long sys_sem_open(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (f->rsi & ~(ull)(SEM_CREATE | SEM_EXCL)) || f->rdx > SEM_VALUE_MAX)
        return -1;
    char name[SEM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !sem_valid_name(name))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int s = sem_open(name, (unsigned)f->rsi, (unsigned)f->rdx);
    if (s < 0)
        return -1;
    p->file[slot].node = PROCESS_SEM_NODE + s;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    sem_ref(s);
    return PROCESS_FD_FIRST + slot;
}

static int sem_of(Process *p, ull fd)
{
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0 || !pfile_is_sem(&p->file[idx]))
        return -1;
    return pfile_sem(&p->file[idx]);
}

/* SYS_SEM_WAIT: rdi = descriptor, rsi = 0 to wait (sleeps while the count is 0), 1 to only try. Takes one off the count: 0.
 * With rsi = 1 and a count of 0: 1 (it would have to wait). -1: not a semaphore descriptor. */
long long sys_sem_wait(const SyscallFrame *f)
{
    int s = sem_of(process_current(), f->rdi);
    if (s < 0 || f->rsi > 1)
        return -1;
    int r;
    while ((r = sem_trywait(s)) == SEM_WOULDBLOCK)
    {
        if (f->rsi == 1)
            return 1;
        sleep_registered(sem_wait_for(s, task_current())); /* a post wakes it; interrupts are off, so the post cannot slip in between the check and here */
        if (sig_interrupted(process_current()))
            return -1;
    }
    return r == 0 ? 0 : -1;
}

/* SYS_SEM_POST: rdi = descriptor: the count goes up by one and a sleeper is woken. 0, or -1 (not a semaphore, or the count is at its
 * maximum). */
long long sys_sem_post(const SyscallFrame *f)
{
    int s = sem_of(process_current(), f->rdi);
    return s >= 0 && sem_post(s) == 0 ? 0 : -1;
}

/* SYS_SEM_VALUE: rdi = descriptor: the count now, or -1. */
long long sys_sem_value(const SyscallFrame *f)
{
    int s = sem_of(process_current(), f->rdi);
    return s >= 0 ? sem_value(s) : -1;
}

long long sys_sem_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[SEM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !sem_valid_name(name))
        return -1;
    return sem_unlink(name);
}

/* ---- mutexes and condition variables ------------------------------------------------------------------------------------------ */
/* SYS_MUTEX_OPEN / SYS_COND_OPEN: rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL). A descriptor for the named object, or -1. SYS_CLOSE lets go;
 * fork copies the descriptor. Names are per kind: a mutex and a condition variable may have the same name. */
static long long usync_open_fd(const SyscallFrame *f, int kind)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (f->rsi & ~(ull)(USYNC_CREATE | USYNC_EXCL)))
        return -1;
    char name[USYNC_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !usync_valid_name(name))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int s = usync_open(name, kind, (unsigned)f->rsi);
    if (s < 0)
        return -1;
    p->file[slot].node = (kind == USYNC_MUTEX ? PROCESS_MUTEX_NODE : PROCESS_COND_NODE) + s;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    usync_ref(s);
    return PROCESS_FD_FIRST + slot;
}

long long sys_mutex_open(const SyscallFrame *f) { return usync_open_fd(f, USYNC_MUTEX); }
long long sys_cond_open(const SyscallFrame *f) { return usync_open_fd(f, USYNC_COND); }

/* The object number behind descriptor fd if it is a mutex (kind USYNC_MUTEX) or condition variable (USYNC_COND), else -1. */
static int usync_of(Process *p, ull fd, int kind)
{
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0)
        return -1;
    if (kind == USYNC_MUTEX ? !pfile_is_mutex(&p->file[idx]) : !pfile_is_cond(&p->file[idx]))
        return -1;
    return pfile_usync(&p->file[idx]);
}

/* SYS_MUTEX_LOCK: rdi = descriptor, rsi = 0 to wait (sleeps while another process holds it), 1 to only try. 0: it is yours now. With rsi = 1
 * and the mutex held: 1. -1: not a mutex, you hold it already, or a signal handler interrupted the wait. */
long long sys_mutex_lock(const SyscallFrame *f)
{
    Process *p = process_current();
    int s = usync_of(p, f->rdi, USYNC_MUTEX);
    if (s < 0 || f->rsi > 1)
        return -1;
    Mutex *m = usync_mutex(s);
    int me = task_current();
    if (m->owner == me)
        return -1; /* it would wait for itself for ever */
    while (mutex_trylock(m) != 0)
    {
        if (f->rsi == 1)
            return 1;
        if (mutex_would_deadlock(m, me))
            return -1; /* the holder (directly or through other mutexes) waits for something this process holds */
        sleep_registered(mutex_wait_for(m, me)); /* an unlock wakes it; interrupts are off, so the unlock cannot slip in between the check and here */
        if (sig_interrupted(p))
        {
            mutex_cancel_wait(m, me);
            return -1;
        }
    }
    mutex_cancel_wait(m, me);
    return 0;
}

/* SYS_MUTEX_UNLOCK: rdi = descriptor. 0, or -1 (not a mutex, or this process does not hold it). */
long long sys_mutex_unlock(const SyscallFrame *f)
{
    int s = usync_of(process_current(), f->rdi, USYNC_MUTEX);
    return s >= 0 && mutex_unlock(usync_mutex(s)) == 0 ? 0 : -1;
}

/* SYS_COND_WAIT: rdi = condition variable descriptor, rsi = mutex descriptor (held by the caller): lets the mutex go and sleeps until a
 * SYS_COND_SIGNAL, then takes the mutex again. 0. -1 if the descriptors are wrong or the mutex is not held by the caller. A signal handler
 * that interrupts the sleep gives -1 as well, but the mutex is held again then, as after 0: check the condition in a loop either way. */
long long sys_cond_wait(const SyscallFrame *f)
{
    Process *p = process_current();
    int sc = usync_of(p, f->rdi, USYNC_COND), sm = usync_of(p, f->rsi, USYNC_MUTEX);
    if (sc < 0 || sm < 0)
        return -1;
    CondVar *c = usync_cond(sc);
    Mutex *m = usync_mutex(sm);
    int me = task_current();
    if (m->owner != me)
        return -1;
    cond_wait_for(c, me); /* registered before the mutex is let go: a signal sent after the unlock cannot be missed */
    mutex_unlock(m);
    int interrupted = 0;
    while (cond_waiting(c, me))
    {
        task_block();
        if (sig_interrupted(p))
        {
            cond_cancel_wait(c, me);
            interrupted = 1;
        }
    }
    mutex_relock(m); /* the mutex comes back whatever woke us (it may sleep until its holder lets go) */
    return interrupted ? -1 : 0;
}

/* SYS_COND_SIGNAL: rdi = descriptor, rsi = 0 wakes one sleeper (the one with the highest priority), 1 wakes all. 0, or -1. A signal with
 * nobody waiting is lost. */
long long sys_cond_signal(const SyscallFrame *f)
{
    int s = usync_of(process_current(), f->rdi, USYNC_COND);
    if (s < 0 || f->rsi > 1)
        return -1;
    if (f->rsi == 0)
        cond_signal(usync_cond(s));
    else
        cond_broadcast(usync_cond(s));
    return 0;
}

/* SYS_SYNC_UNLINK: rdi = name, rsi = 0 for a mutex, 1 for a condition variable: the name goes away; the object lives while a descriptor is
 * open. 0, or -1. */
long long sys_sync_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current() || f->rsi > 1)
        return -1;
    char name[USYNC_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !usync_valid_name(name))
        return -1;
    return usync_unlink(name, (int)f->rsi);
}

