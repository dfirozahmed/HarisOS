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
#include "vfs.h"

typedef unsigned long long u64;

/* The file of a demand paged program (process_lazy.c): what the pages are loaded from. */
#define LAZY_IO_BYTES (8 * 4096) /* the most one page fault reads in one go */
typedef struct LazyImage
{
    int node;      /* VFS node of the file */
    unsigned size; /* its size and name when the process started: checked at every load */
    char name[VFS_NAME_MAX];
    int nseg;
    ElfSegment seg[ELF_MAX_SEGMENTS];
    unsigned reads, pages, bytes; /* what the loads did so far (printed when the process ends): file reads, pages made, bytes read */
    unsigned long long ms;        /* time spent in them, ms */
    unsigned char io[LAZY_IO_BYTES]; /* the bytes of a read pass through here on their way into the new pages */
} LazyImage;

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

/* ---- process_lazy.c ---- */
void process_lazy_ensure(const AddressSpace *as, u64 va, u64 len); /* the kernel is about to use [va, va+len): load the pages of it that are not there yet */
int process_lazy_populate_all(Process *p, AddressSpace *as);       /* load every page, then forget the image (before a fork); 0, or -1 */
void process_lazy_free(Process *p);
void process_lazy_report(const Process *p); /* one serial line: how much a demand paged program loaded and how long it took */

/* ---- process_exit.c ---- */
void purge_exits(int owner);

#endif
