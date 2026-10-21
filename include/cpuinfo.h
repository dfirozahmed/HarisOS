/* ============================================================
 * HarisOS CPU information: name, base speed, sockets, cores, logical processors.
 *
 *   cpuinfo_probe(cpuinfo_hw_cpuid, apic_ids, count, tsc_khz);   once, after acpi_init() and timer_init()
 *   cpuinfo_get()                                                 the result
 *
 * How the numbers are found:
 *  - Logical processors = the enabled processors in the ACPI MADT (acpi_cpu_apic_id). Without a MADT, only the
 *    package this CPU sits in is known, from CPUID.
 *  - An APIC id is a bit field: [ socket | core | SMT thread ]. CPUID tells how many low bits are the thread
 *    (smt_shift) and how many are thread + core (pkg_shift); then cores = distinct (id >> smt_shift) and
 *    sockets = distinct (id >> pkg_shift). Counting ids, not multiplying, keeps mixed CPUs right (Intel
 *    P-cores have two threads, E-cores one).
 *  - Base speed = CPUID leaf 0x16 (Intel), else the "@ 3.40GHz" in the brand string, else the TSC speed
 *    measured by the timer (that is the base clock on most CPUs; base_estimated = 1 says it came from there).
 *
 * The CPUID instruction is passed in as a function so the decoding can be tested on the host.
 * ============================================================ */
#ifndef HARIS_CPUINFO_H
#define HARIS_CPUINFO_H

/* r[0..3] = EAX, EBX, ECX, EDX after CPUID(leaf, sub) */
typedef void (*CpuidFn)(unsigned leaf, unsigned sub, unsigned r[4]);
void cpuinfo_hw_cpuid(unsigned leaf, unsigned sub, unsigned r[4]);

typedef struct
{
    char vendor[13];  /* "GenuineIntel", "AuthenticAMD" ... */
    char brand[49];   /* "Intel(R) Core(TM) ... @ 3.40GHz", or "" */
    unsigned base_mhz; /* 0 = unknown */
    int base_estimated; /* 1 = taken from the measured TSC speed, not from the CPU */
    int sockets, cores, logical;
} CpuInfo;

void cpuinfo_probe(CpuidFn cpuid, const unsigned *apic_ids, int apic_count, unsigned tsc_khz);
const CpuInfo *cpuinfo_get(void);

#endif
