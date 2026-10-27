/* Shared by the files of the process code (kernel/proc/process/). The public interface is include/process.h. */
#ifndef HARIS_PROCESS_INTERNAL_H
#define HARIS_PROCESS_INTERNAL_H

#include "process.h"
#include "aspace.h"
#include "bkl.h"
#include "console.h"
#include "elf.h"
#include "gui.h"
#include "heap.h"
#include "idle.h"
#include "pmm.h"
#include "segments.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"

typedef unsigned long long u64;

/* ---- types, constants and variables used by more than one file ---- */
typedef struct
{
    u64 entry, user_rsp, argc, argv;
} Start;

__attribute__((noreturn)) void process_enter_user(u64 entry, u64 user_rsp, u64 argc, u64 argv);

/* ---- process_core.c ---- */
extern ProcessStats STATS;

/* ---- process_launch.c ---- */
void user_thread_main(void *arg);
void process_free(void *p);
extern int NEXT_PID;
unsigned stack_pages_for(const ProcessSpawn *sp);
int place_args(AddressSpace *as, const ProcessSpawn *sp, Start *st);
void set_heap(Process *p, u64 heap_base);
void purge_exits(int owner);
int load_segments(AddressSpace *as, const ElfInfo *info, const void *file, u64 *heap_base);

/* ---- process_exit.c ---- */
void purge_exits(int owner);

#endif
