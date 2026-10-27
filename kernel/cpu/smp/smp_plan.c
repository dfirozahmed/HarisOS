/* SMP: which processors to start, the trampoline image and the page tables it needs (host-tested, pure functions). */
#include "smp_internal.h"

/* HarisOS SMP, stage 1: see include/smp.h. */
static void put16(u8 *p, unsigned v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, unsigned v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void put64(u8 *p, u64 v)
{
    put32(p, (unsigned)v);
    put32(p + 4, (unsigned)(v >> 32));
}

int smp_pick_aps(const unsigned *apic_ids, int n, unsigned bsp_id, unsigned max_id, unsigned *out, int max)
{
    int count = 0;
    for (int i = 0; i < n && count < max; i++)
    {
        unsigned id = apic_ids[i];
        if (id == bsp_id || id > max_id)
            continue;
        int seen = 0;
        for (int j = 0; j < count; j++)
            if (out[j] == id)
                seen = 1;
        if (!seen)
            out[count++] = id;
    }
    return count;
}

void smp_patch_image(u8 *img, unsigned code_phys, unsigned data_phys, unsigned pml4_phys, unsigned pm32_off, unsigned lm64_off)
{
    /* The trampoline GDT. 0x08: 64-bit code (L = 1); 0x10: flat 32-bit data; 0x18: flat 32-bit code. */
    put64(img + SMP_OFF_GDT + 0, 0);
    put64(img + SMP_OFF_GDT + 8, 0x00209A0000000000ULL);
    put64(img + SMP_OFF_GDT + 16, 0x00CF92000000FFFFULL);
    put64(img + SMP_OFF_GDT + 24, 0x00CF9A000000FFFFULL);

    put16(img + SMP_OFF_GDTR, 4 * 8 - 1); /* the real mode code loads this with a 32-bit base, so the page's physical address */
    put32(img + SMP_OFF_GDTR + 2, code_phys + SMP_OFF_GDT);
    put16(img + SMP_OFF_GDTR + 6, 0);

    put32(img + SMP_OFF_PM_PTR, code_phys + pm32_off);
    put16(img + SMP_OFF_PM_PTR + 4, 0x18);
    put16(img + SMP_OFF_PM_PTR + 6, 0);

    put32(img + SMP_OFF_LM_PTR, code_phys + lm64_off);
    put16(img + SMP_OFF_LM_PTR + 4, 0x08);
    put16(img + SMP_OFF_LM_PTR + 6, 0);

    put32(img + SMP_OFF_DATA, data_phys);
    put32(img + SMP_OFF_PML4, pml4_phys);
}

void smp_build_tables(u64 *pml4, u64 *pdpt, u64 *pd, u64 pdpt_phys, u64 pd_phys)
{
    for (int i = 0; i < 512; i++)
    {
        pml4[i] = 0;
        pdpt[i] = 0;
        pd[i] = 0;
    }
    pml4[0] = pdpt_phys | 3;  /* present, writable */
    pdpt[0] = pd_phys | 3;
    pd[0] = 0x83;             /* present, writable, 2 MiB page at 0 */
}
