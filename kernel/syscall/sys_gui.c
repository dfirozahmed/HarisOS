/* System calls: GUI windows (gui.h). */
#include "syscall_internal.h"

/* ---- GUI windows (gui.h) ------------------------------------------------------------------------------------------------------ */
/* The surface a handle from ring 3 names, if it is this process's own; its handle, or -1. (A handle is a positive int.) */
static int gui_mine(const Process *p, ull h)
{
    if (!p || h == 0 || h > 0x7FFFFFFFull)
        return -1;
    return gui_owner((int)h) == p->pid ? (int)h : -1;
}

/* SYS_GUI_CREATE: rdi = pointer to a SysGuiInfo (title, width, height in; handle, stride, pixels out). The surface's pixels are a shared
 * memory segment, mapped here like SYS_SHM_MAP does it; the window itself appears on the desktop later, from the main loop. 0, or -1. */
long long sys_gui_create(const SyscallFrame *f)
{
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, f->rdi, sizeof(SysGuiInfo), 1))
        return -1;
    SysGuiInfo info;
    copy_from_user((char *)&info, f->rdi, sizeof(info));
    if (info.title[sizeof(info.title) - 1] != 0) /* not NUL-terminated within its 32 bytes */
        return -1;
    ProcessShm *slot = process_shm_slot(p); /* a free entry; the list grows when all are in use */
    if (!slot)
        return -1;
    int seg;
    int h = gui_create(info.title, info.width, info.height, p->pid, &seg);
    if (h < 0)
        return -1;
    ull addr = shm_attach(p, as, slot, seg);
    if (!addr)
    {
        gui_destroy(h); /* the only holder of the pixels: they are freed */
        return -1;
    }
    info.handle = h;
    info.stride = info.width;
    info.pixels = addr;
    copy_to_user(f->rdi, (const char *)&info, sizeof(info));
    return 0;
}

/* SYS_GUI_PRESENT: rdi = handle, rsi = x | y << 16, rdx = w | h << 16. */
long long sys_gui_present(const SyscallFrame *f)
{
    int h = gui_mine(process_current(), f->rdi);
    if (h < 0)
        return -1;
    return gui_present(h, (int)(f->rsi & 0xFFFF), (int)((f->rsi >> 16) & 0xFFFF), (int)(f->rdx & 0xFFFF), (int)((f->rdx >> 16) & 0xFFFF));
}

/* SYS_GUI_EVENT: rdi = handle, rsi = pointer to a GuiEvent, rdx = SYS_GUI_NOWAIT or 0. Sleeps like a pipe read until an event is there. */
long long sys_gui_event(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    int h = gui_mine(p, f->rdi);
    if (h < 0 || !as || !aspace_user_ok(as, f->rsi, sizeof(GuiEvent), 1))
        return -1;
    GuiEvent ev;
    int r;
    while ((r = gui_next_event(h, &ev)) == 0)
    {
        if (f->rdx & SYS_GUI_NOWAIT)
            return 0;
        gui_wait(h, task_current());
        task_block();
        if (sig_interrupted(p))
            return -1;
    }
    if (r < 0)
        return -1; /* the window was destroyed while we slept */
    copy_to_user(f->rsi, (const char *)&ev, sizeof(ev));
    return 1;
}

/* SYS_GUI_CTL: rdi = handle, rsi = SYS_GUI_DESTROY, or SYS_GUI_SET_TITLE with rdx = the new title. */
long long sys_gui_ctl(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    int h = gui_mine(process_current(), f->rdi);
    if (h < 0 || !as)
        return -1;
    if (f->rsi == SYS_GUI_DESTROY)
    {
        gui_destroy(h);
        return 0;
    }
    if (f->rsi == SYS_GUI_SET_TITLE)
    {
        char title[GUI_TITLE_MAX + 1];
        if (copy_string_from_user(title, (int)sizeof(title), f->rdx, as) < 0)
            return -1;
        return gui_set_title(h, title);
    }
    return -1;
}

/* The heap. Pages are mapped one by one up to the new break; if memory runs out half way, the pages already mapped stay
 * (heap_mapped moves with them, so a retry does not map them twice) but the break does not move. */
