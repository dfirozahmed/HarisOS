/* HarisOS CPU information: see include/cpuinfo.h. */
#include "cpuinfo.h"

static CpuInfo INFO;

#if defined(__x86_64__) || defined(__i386__)
void cpuinfo_hw_cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
#else
void cpuinfo_hw_cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    (void)leaf;
    (void)sub;
    r[0] = r[1] = r[2] = r[3] = 0;
}
#endif

static void put_word(char *dst, unsigned w)
{
    for (int i = 0; i < 4; i++)
        dst[i] = (char)(w >> (8 * i));
}

static unsigned ceil_log2(unsigned n)
{
    unsigned s = 0;
    while (s < 31 && (1u << s) < n)
        s++;
    return s;
}

/* "3.40GHz" / "2.60 GHz" / "800MHz" in the brand string -> MHz, 0 if there is none */
static unsigned mhz_from_brand(const char *b)
{
    for (int i = 1; b[i] && b[i + 1] && b[i + 2]; i++)
    {
        unsigned unit = 0;
        if (b[i] == 'G' && b[i + 1] == 'H' && b[i + 2] == 'z')
            unit = 1000;
        else if (b[i] == 'M' && b[i + 1] == 'H' && b[i + 2] == 'z')
            unit = 1;
        if (!unit)
            continue;
        int j = i - 1;
        if (j >= 0 && b[j] == ' ')
            j--;
        int end = j;
        while (j >= 0 && ((b[j] >= '0' && b[j] <= '9') || b[j] == '.'))
            j--;
        if (end == j)
            continue;
        unsigned whole = 0, frac = 0, frac_scale = 1;
        int in_frac = 0;
        for (int k = j + 1; k <= end; k++)
        {
            if (b[k] == '.')
                in_frac = 1;
            else if (!in_frac)
                whole = whole * 10 + (unsigned)(b[k] - '0');
            else if (frac_scale < 1000)
            {
                frac = frac * 10 + (unsigned)(b[k] - '0');
                frac_scale *= 10;
            }
        }
        return whole * unit + frac * unit / frac_scale;
    }
    return 0;
}

/* how many distinct values (id >> shift) there are among the ids */
static int count_distinct(const unsigned *ids, int n, unsigned shift)
{
    int found = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned key = shift >= 32 ? 0 : ids[i] >> shift;
        int seen = 0;
        for (int k = 0; k < i && !seen; k++)
            seen = (shift >= 32 ? 0 : ids[k] >> shift) == key;
        if (!seen)
            found++;
    }
    return found;
}

void cpuinfo_probe(CpuidFn cpuid, const unsigned *apic_ids, int apic_count, unsigned tsc_khz)
{
    unsigned r[4];
    CpuInfo *c = &INFO;
    for (unsigned i = 0; i < sizeof *c; i++)
        ((volatile char *)c)[i] = 0;

    cpuid(0, 0, r);
    unsigned max_leaf = r[0];
    put_word(c->vendor, r[1]);
    put_word(c->vendor + 4, r[3]);
    put_word(c->vendor + 8, r[2]);
    int intel = c->vendor[0] == 'G' && c->vendor[7] == 'I'; /* GenuineIntel */

    cpuid(0x80000000u, 0, r);
    unsigned max_ext = r[0];
    if (max_ext >= 0x80000004u)
    {
        for (unsigned k = 0; k < 3; k++)
        {
            cpuid(0x80000002u + k, 0, r);
            for (int w = 0; w < 4; w++)
                put_word(c->brand + k * 16 + w * 4, r[w]);
        }
        c->brand[48] = 0;
        int skip = 0;
        while (c->brand[skip] == ' ')
            skip++;
        if (skip)
            for (int i = 0; i + skip <= 48; i++)
                c->brand[i] = c->brand[i + skip];
    }

    /* base speed */
    if (max_leaf >= 0x16)
    {
        cpuid(0x16, 0, r);
        c->base_mhz = r[0] & 0xFFFF;
    }
    if (!c->base_mhz)
        c->base_mhz = mhz_from_brand(c->brand);
    if (!c->base_mhz && tsc_khz)
    {
        c->base_mhz = tsc_khz / 1000;
        c->base_estimated = 1;
    }

    /* topology: smt_shift = low APIC id bits that pick the thread, pkg_shift = bits that pick thread + core */
    unsigned smt_shift = 0, pkg_shift = 32; /* 32 = unknown: every processor counts as one package */
    unsigned per_pkg = 1;                   /* logical processors in this CPU's package */
    int topo = 0;

    cpuid(1, 0, r);
    unsigned leaf1_logical = (r[3] & (1u << 28)) ? (r[1] >> 16) & 0xFF : 1;
    if (leaf1_logical == 0)
        leaf1_logical = 1;

    if (max_leaf >= 0xB)
    {
        int saw_smt = 0;
        for (unsigned sub = 0; sub < 8; sub++)
        {
            cpuid(0xB, sub, r);
            unsigned type = (r[2] >> 8) & 0xFF, count = r[1] & 0xFFFF;
            if (type == 0 || count == 0)
                break;
            if (type == 1)
            {
                smt_shift = r[0] & 0x1F;
                saw_smt = 1;
            }
            else if (type == 2)
            {
                pkg_shift = r[0] & 0x1F;
                per_pkg = count;
                topo = 1;
            }
        }
        if (!saw_smt)
            topo = 0;
    }
    if (!topo && !intel && max_ext >= 0x80000008u)
    {
        unsigned threads_per_core = 1;
        cpuid(0x80000001u, 0, r);
        if ((r[2] & (1u << 22)) && max_ext >= 0x8000001Eu) /* TOPOEXT */
        {
            cpuid(0x8000001Eu, 0, r);
            threads_per_core = ((r[1] >> 8) & 0xFF) + 1;
        }
        cpuid(0x80000008u, 0, r);
        per_pkg = (r[2] & 0xFF) + 1;
        unsigned id_bits = (r[2] >> 12) & 0xF;
        smt_shift = ceil_log2(threads_per_core);
        pkg_shift = id_bits ? id_bits : ceil_log2(per_pkg);
        topo = 1;
    }
    if (!topo)
    {
        per_pkg = leaf1_logical;
        unsigned cores_per_pkg = per_pkg;
        if (intel && max_leaf >= 4)
        {
            cpuid(4, 0, r);
            cores_per_pkg = (r[0] >> 26) + 1;
        }
        if (cores_per_pkg == 0 || cores_per_pkg > per_pkg)
            cores_per_pkg = per_pkg;
        smt_shift = ceil_log2(per_pkg / cores_per_pkg);
        pkg_shift = ceil_log2(per_pkg);
        topo = 1;
    }

    if (apic_ids && apic_count > 0)
    {
        c->logical = apic_count;
        c->cores = count_distinct(apic_ids, apic_count, smt_shift);
        c->sockets = count_distinct(apic_ids, apic_count, pkg_shift);
    }
    else /* no MADT: only this package is known */
    {
        c->logical = (int)per_pkg;
        c->cores = (int)(per_pkg >> smt_shift);
        if (c->cores < 1)
            c->cores = 1;
        c->sockets = 1;
    }
}

const CpuInfo *cpuinfo_get(void)
{
    return &INFO;
}
