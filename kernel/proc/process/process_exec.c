/* Processes: exec and fork. */
#include "process_internal.h"

/* ---- exec: the running process becomes another program ------------------------------------------------------------------- */
/* Builds the new program's address space (segments, stack, arguments) first; only when that has worked does it let go of the
 * old one. So a file that is not a program, or not enough memory, is an error the caller can handle, with everything as it was.
 * What stays: the pid, the parent, the console, the working folder, the open files and redirections, ignored signals.
 * It does not return when it works: the thread goes on in ring 3 at the new program's entry. */
int process_exec_elf(const char *name, const void *file, u64 size, const ProcessSpawn *args, int *err, int free_file)
{
    Process *p = process_current();
    if (!p)
        return -1;
    ElfInfo info;
    int e = elf_parse(file, size, &info);
    if (err)
        *err = e;
    if (e != ELF_OK)
        return -1;
    AddressSpace *as = aspace_create();
    if (!as)
        return -1;
    u64 heap_base;
    if (load_segments(as, &info, file, &heap_base) != 0)
    {
        aspace_destroy(as);
        return -1;
    }
    unsigned stack_pages = stack_pages_for(args);
    for (unsigned i = 1; i <= stack_pages; i++)
        if (aspace_map_new(as, PAGING_USER_END - (u64)i * 4096, ASPACE_WRITE) != 0)
        {
            aspace_destroy(as);
            return -1;
        }
    Start st;
    st.entry = info.entry;
    st.user_rsp = PAGING_USER_END - 8;
    st.argc = st.argv = 0;
    if (place_args(as, args, &st) != 0)
    {
        aspace_destroy(as);
        return -1;
    }
    /* no way back from here. The file's bytes are in the new pages now: the buffer they came from goes back to the heap before the
     * old program does, since nothing after this point returns to the caller that would free it. */
    if (free_file)
        kfree((void *)file);
    if (args->free_args)
        kfree((void *)args->args); /* the arguments are on the new stack now */
    process_release_shm(p); /* the old address space (and the mappings in it) goes away below */
    gui_release_owner(p->pid); /* its windows drew into those mappings */
    for (int i = 0; i < 32; i++) /* a new program has none of the old one's handlers (ignored signals stay ignored) */
        p->sig_handler[i] = 0;
    p->sig_entry = 0;
    p->sig_pending = p->sig_blocked = 0;
    set_heap(p, heap_base);
    p->stack_low = PAGING_USER_END - (u64)stack_pages * 4096;
    task_exec_replace(as, name);
    process_enter_user(st.entry, st.user_rsp, st.argc, st.argv);
}

/* ---- fork: a copy of the running process -------------------------------------------------------------------------------- */
/* The child gets its own copy of every page (aspace_clone), a copy of the Process record (same console, folder, open files and
 * redirections, ignored signals; the pipes it holds are counted for it), and a thread that goes on at user_rip with user_rsp,
 * as if SYSCALL had returned there, with rax = 0. Its other registers are NOT the parent's: only a caller that saved them (the
 * fork() of the user library does) can use the child. Returns the child's pid (the parent's return value) or -1: no slot for
 * another child, no memory, no thread. */
int process_fork(u64 user_rip, u64 user_rsp)
{
    Process *p = process_current();
    AddressSpace *as = task_address_space();
    if (!p || !as)
        return -1;
    int slot = process_child_slot(p);
    if (slot < 0)
        return -1;
    AddressSpace *cas = aspace_clone(as);
    if (!cas)
        return -1;
    Start *st = kmalloc(sizeof(Start));
    Process *cp = kmalloc(sizeof(Process));
    if (!st || !cp)
    {
        kfree(st);
        kfree(cp);
        aspace_destroy(cas);
        return -1;
    }
    {
        const volatile unsigned char *from = (const volatile unsigned char *)p; /* (no memcpy in a freestanding build) */
        volatile unsigned char *to = (volatile unsigned char *)cp;
        for (unsigned i = 0; i < sizeof(Process); i++)
            to[i] = from[i];
    }
    if (process_files_copy(cp, p) != 0) /* the clone gets a list of open files of its own too (entries are copies, as before) */
    {
        kfree(st);
        kfree(cp);
        aspace_destroy(cas);
        return -1;
    }
    cp->shm = 0; /* the clone gets a list of its own: it maps the same segments, but unmaps them independently */
    cp->shm_cap = 0;
    if (p->shm_cap > 0)
    {
        cp->shm = kmalloc((unsigned long long)p->shm_cap * sizeof(ProcessShm));
        if (!cp->shm)
        {
            kfree(cp->file);
            kfree(st);
            kfree(cp);
            aspace_destroy(cas);
            return -1;
        }
        for (int i = 0; i < p->shm_cap; i++)
            cp->shm[i] = p->shm[i];
        cp->shm_cap = p->shm_cap;
    }
    cp->pid = NEXT_PID;
    cp->parent = task_current();
    cp->own_console = 0; /* the window belongs to whoever opened it */
    cp->sig_pending = 0; /* handlers and the mask are inherited, what is waiting for the parent is not */
    cp->child = 0; /* the clone has no children yet */
    cp->child_cap = 0;
    st->entry = user_rip;
    st->user_rsp = user_rsp;
    st->argc = st->argv = 0;
    int id = task_create_user(task_name(task_current()), user_thread_main, st, cas, cp, process_free);
    if (id < 0)
    {
        kfree(st);
        kfree(cp->file);
        kfree(cp->shm);
        kfree(cp);
        aspace_destroy(cas);
        return -1;
    }
    for (int i = 0; i < cp->file_cap; i++) /* the child holds the pipe ends it inherited */
        pfile_ref(&cp->file[i]);
    for (int i = 0; i < cp->shm_cap; i++) /* ... and the shared memory it inherited (the pages are shared by the clone) */
        if (cp->shm[i].seg >= 0)
            shm_ref(cp->shm[i].seg);
    STATS.started++;
    p->child[slot] = NEXT_PID;
    return NEXT_PID++;
}

int process_cow_fault(u64 addr)
{
    AddressSpace *as = task_address_space();
    return as ? aspace_cow_fault(as, addr) : 0;
}
