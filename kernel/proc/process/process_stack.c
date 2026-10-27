/* Processes: the stack grows. */
#include "process_internal.h"

/* ---- the stack grows ---------------------------------------------------------------------------------------------------- */
/* Map the stack down to the page of `addr` (which must be in the area the stack may grow into, below stack_low). The pages in between are
 * mapped too, so the stack stays one piece. 1 = grew, 0 = nothing to do (already mapped, or not in the stack area), -1 = no memory (what
 * was mapped stays, stack_low follows it). */
static int grow_stack_to(Process *p, AddressSpace *as, u64 addr)
{
    u64 area_low = PAGING_USER_END - ELF_STACK_RESERVE;
    if (addr < area_low || addr >= p->stack_low)
        return 0;
    u64 want = addr & ~4095ULL;
    while (p->stack_low > want)
    {
        if (aspace_map_new(as, p->stack_low - 4096, ASPACE_WRITE) != 0)
            return -1;
        p->stack_low -= 4096;
    }
    return 1;
}

/* A push, a call or a store into a new stack frame lies at most this far below the stack pointer (a frame with big arrays is entered with
 * one subtraction; probing it a page at a time is what a compiler does for frames bigger than this). */
#define STACK_SLACK (64 * 1024)

int process_stack_fault(u64 addr, u64 rsp)
{
    Process *p = process_current();
    AddressSpace *as = task_address_space();
    if (!p || !as || p->stack_low == 0 || addr + STACK_SLACK < rsp)
        return 0;
    return grow_stack_to(p, as, addr) == 1;
}

int process_stack_grow(u64 addr)
{
    Process *p = process_current();
    AddressSpace *as = task_address_space();
    if (!p || !as || p->stack_low == 0)
        return 0;
    u64 area_low = PAGING_USER_END - ELF_STACK_RESERVE;
    if (addr < area_low || addr >= PAGING_USER_END)
        return 0;
    if (addr >= p->stack_low)
        return 1;
    return grow_stack_to(p, as, addr) >= 0;
}

int process_ppid(void)
{
    const Process *p = process_current();
    const Process *pp = p && p->parent > 0 ? task_data_of(p->parent) : 0;
    return pp ? pp->pid : 0;
}

/* The exit codes of children that their parent has not collected yet. The pid is never reused, so a stale record can never be mistaken
 * for a newer process. Only children are recorded. The records are a heap block that grows as needed (up to ZOMBIE_MAX; at the limit the
 * oldest is overwritten, as the ring of 32 used to do for everybody); a record goes when its child is waited for, or when its parent ends
 * (process_release_files): nobody can collect it then. A free entry has pid 0. */
