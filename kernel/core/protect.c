/* HarisOS CPU protections (SMEP, SMAP): see include/protect.h. */
#include "protect.h"

unsigned long long protect_cr4(unsigned cpuid7_ebx, unsigned long long cr4)
{
    if (cpuid7_ebx & (1u << 7))
        cr4 |= CR4_SMEP;
    if (cpuid7_ebx & (1u << 20))
        cr4 |= CR4_SMAP;
    return cr4;
}

#if defined(__x86_64__) && !defined(HARIS_HOST)

#include "aspace.h"
#include "fault.h"
#include "idle.h"
#include "irq.h"
#include "pmm.h"
#include "uaccess.h"

typedef unsigned long long u64;

int smap_enabled; /* read by uaccess.h */
static int STATUS;

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static u64 read_cr4(void)
{
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

void protect_init(void)
{
    unsigned r[4];
    cpuid(0, 0, r);
    if (r[0] < 7)
        return; /* no leaf 7: an old CPU with neither */
    cpuid(7, 0, r);
    u64 cr4 = read_cr4();
    u64 want = protect_cr4(r[1], cr4);
    if (want != cr4)
        __asm__ volatile("mov %0, %%cr4" : : "r"(want) : "memory");
    cr4 = read_cr4(); /* what the CPU actually kept */
    STATUS = ((cr4 & CR4_SMEP) ? PROTECT_SMEP : 0) | ((cr4 & CR4_SMAP) ? PROTECT_SMAP : 0);
    smap_enabled = (cr4 & CR4_SMAP) != 0;
}

int protect_status(void) { return STATUS; }

static void nop_function(void) {}

/* The page-fault error code: bit 0 = the page was present (a protection violation, not a missing page), bit 1 = write,
 * bit 2 = the access came from ring 3, bit 4 = it was an instruction fetch. */
#define PF_PRESENT 1u
#define PF_WRITE 2u
#define PF_USER 4u
#define PF_FETCH 16u

static int user_page_tests(int status)
{
    u64 free0 = pmm_free_pages();
    AddressSpace *as = aspace_create();
    if (!as)
        return -1;
    const u64 va = PAGING_USER_BASE + 0x2000;
    u64 phys;
    if (aspace_map_new(as, va, ASPACE_EXEC) != 0 || aspace_translate(as, va, &phys) != 0)
    {
        aspace_destroy(as);
        return -1;
    }
    *(volatile unsigned char *)phys = 0xC3; /* "ret": a function that does nothing, on a user page */
    *(volatile unsigned char *)(phys + 16) = 0x5A;

    int result = 0;
    FaultProbe p;
    u64 irq = irq_save(); /* the whole test runs with the new space loaded and interrupts off */
    aspace_switch(as);

    /* controls: the probes themselves work on ordinary kernel memory, so a failure below is the protection's doing */
    static volatile int marker = 1;
    if (!fault_probe_read((u64)&marker, 0))
        result = -2;
    else if (!fault_probe_exec((u64)nop_function, 0))
        result = -3;

    if (!result && (status & PROTECT_SMAP))
    {
        if (fault_probe_read(va + 16, &p))
            result = -4; /* the kernel read a user page without opening the window */
        else if (p.cr2 != va + 16 || (p.error & (PF_PRESENT | PF_WRITE | PF_USER | PF_FETCH)) != PF_PRESENT)
            result = -5; /* it faulted, but not as a protection violation of a kernel read */
        else
        {
            user_access_begin();
            int inside = fault_probe_read(va + 16, 0);
            user_access_end();
            if (!inside)
                result = -6; /* the window does not open */
            else if (fault_probe_read(va + 16, 0))
                result = -7; /* ... and does not close */
        }
    }

    if (!result && (status & PROTECT_SMEP))
    {
        /* with the SMAP window open too: SMEP is about instruction fetches and does not care about AC */
        user_access_begin();
        int ran = fault_probe_exec(va, &p);
        user_access_end();
        if (ran)
            result = -8; /* the kernel executed code on a user page */
        else if (p.cr2 != va || (p.error & (PF_PRESENT | PF_USER | PF_FETCH)) != (PF_PRESENT | PF_FETCH))
            result = -9;
    }

    aspace_switch(0);
    irq_restore(irq);
    aspace_destroy(as);
    if (!result && pmm_free_pages() != free0)
        result = -10;
    return result;
}

int protect_selftest(void)
{
    int status = protect_status();
    if (status)
    {
        int r = user_page_tests(status);
        if (r)
            return r;
    }

    /* An NMI to ourselves must arrive, and arrive on its own stack (IST3), not on the stack we are using. */
    if (idle_ready())
    {
        unsigned n0 = fault_nmi_count();
        idle_send_self_nmi();
        for (int i = 0; i < 1000000 && fault_nmi_count() == n0; i++)
            __asm__ volatile("pause");
        if (fault_nmi_count() == n0)
            return -20;
        if (!fault_nmi_on_own_stack())
            return -21;
    }
    return 0;
}

#else

void protect_init(void) {}
int protect_status(void) { return 0; }
int protect_selftest(void) { return 0; }

#endif
