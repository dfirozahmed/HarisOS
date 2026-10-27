/* Shared by the files of the program loader (kernel/proc/exec/). The public interface is include/exec.h. */
#ifndef HARIS_EXEC_INTERNAL_H
#define HARIS_EXEC_INTERNAL_H

#include "exec.h"
#include "console.h"
#include "elf.h"
#include "heap.h"
#include "cow.h"
#include "paging.h"
#include "mq.h"
#include "pipe.h"
#include "sem.h"
#include "shm.h"
#include "pmm.h"
#include "process.h"
#include "serial.h"
#include "task.h"
#include "vfs.h"

typedef unsigned long long u64;

/* ---- exec_core.c ---- */
unsigned char *read_whole(int node, unsigned *size_out, int *err);

#endif
