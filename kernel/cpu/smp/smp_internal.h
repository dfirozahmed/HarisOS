/* Shared by the files of the SMP code (kernel/cpu/smp/). The public interface is include/smp.h. */
#ifndef HARIS_SMP_INTERNAL_H
#define HARIS_SMP_INTERNAL_H

#include "smp.h"

typedef unsigned long long u64;
typedef unsigned char u8;

#if defined(__x86_64__) && !defined(HARIS_HOST)

#include "acpi.h"
#include "gdt.h"
#include "idle.h"
#include "bkl.h"
#include "heap.h"
#include "percpu.h"
#include "pmm.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"

/* ---- types, constants and variables used by more than one file ---- */
#define MSR_EFER 0xC0000080u

#define EFER_LMA (1ULL << 10)

#define MSR_PAT 0x277u

/* ---- smp_start.c ---- */
typedef struct
{
    unsigned int reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    unsigned short reserved3;
    unsigned short iomap_base;
} __attribute__((packed)) ApTss;

typedef struct
{
    PerCpu pc;               /* first: gs base of this processor (percpu.h); pc.apic_id is what the MADT says */
    u64 gdt[GDT_ENTRIES];
    ApTss tss;
    unsigned lapic_id;       /* read by the processor itself once it runs */
    volatile unsigned online;
    void (*volatile job)(void); /* set by smp_run_on, picked up by the processor within one timer tick */
    volatile int job_done;
} ApCpu;

extern ApCpu CPU[SMP_MAX_CPUS];
extern u8 AP_STACK[SMP_MAX_CPUS][SMP_STACK_SIZE] __attribute__((aligned(16)));
extern int TOTAL;
u64 rdmsr(unsigned msr);
void wait_us(unsigned us);
extern const volatile SmpParams *PARAMS;
void ap_entry(u64 index);

#endif /* x86-64 kernel build */

#endif
