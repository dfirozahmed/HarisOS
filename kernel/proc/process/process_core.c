/* Processes: statistics, exit code, waiting. */
#include "process_internal.h"

/* HarisOS user processes: see include/process.h. */

ProcessStats STATS;

void process_stats(ProcessStats *out) { *out = STATS; }

/* (the exit code of a process is kept in the record of its thread, see task_set_exit_code) */
int process_exit_code(int id) { return task_exit_code(id); }

int process_wait(int id)
{
    for (int i = 0; i < 400; i++)
    {
        TaskState s = task_state(id);
        if (s == TASK_DEAD || s == TASK_UNUSED)
            break;
        task_yield(); /* it runs until it traps, is preempted by the timer, or ends */
    }
    task_yield(); /* frees the stack, the pages and the page tables of the finished thread */
    task_yield();
    return task_state(id) == TASK_UNUSED ? 0 : -1;
}

/* Leave ring 0 for ring 3 with IRETQ: it pops RIP, CS, RFLAGS, RSP, SS, so build that frame and go. RFLAGS = 0x202:
 * interrupts on (the timer must be able to take the CPU from a process that never gives it up), bit 1 is always set.
 * The general registers are cleared so no kernel value is handed to user code. Arguments: rcx = entry, rdx = user RSP, r8 = argc, r9 = argv (they arrive in rcx, rdx and in rdi, rsi: Microsoft and System V calling conventions). */
