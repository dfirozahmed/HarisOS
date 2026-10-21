/* Host test for cpuinfo.c (CPUID decoding on fake CPUs) and the ACPI MADT part of acpi.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acpi.h"
#include "cpuinfo.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- a fake CPU: only the fields the scenario sets are answered, everything else reads 0 ---- */
typedef struct
{
    const char *vendor;
    const char *brand;
    unsigned max_leaf, max_ext;
    unsigned leaf1_ebx, leaf1_edx;
    unsigned leaf4_eax;
    unsigned base_mhz;                 /* leaf 0x16 */
    int nb_levels;                     /* leaf 0xB: levels[0] = SMT, levels[1] = core */
    struct { unsigned shift, count, type; } lv[2];
    unsigned ext1_ecx;                 /* 0x80000001 */
    unsigned amd_1e_ebx;               /* 0x8000001E */
    unsigned amd_08_ecx;               /* 0x80000008 */
} Fake;
static const Fake *CPU;

static unsigned word(const char *s, int off)
{
    unsigned w = 0;
    for (int i = 0; i < 4; i++)
        w |= (unsigned)(unsigned char)s[off + i] << (8 * i);
    return w;
}

static void fake_cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    r[0] = r[1] = r[2] = r[3] = 0;
    if (leaf == 0) {
        r[0] = CPU->max_leaf;
        r[1] = word(CPU->vendor, 0); r[3] = word(CPU->vendor, 4); r[2] = word(CPU->vendor, 8);
    } else if (leaf == 1) {
        r[1] = CPU->leaf1_ebx; r[3] = CPU->leaf1_edx;
    } else if (leaf == 4) {
        r[0] = CPU->leaf4_eax;
    } else if (leaf == 0xB) {
        if ((int)sub < CPU->nb_levels) {
            r[0] = CPU->lv[sub].shift; r[1] = CPU->lv[sub].count; r[2] = (CPU->lv[sub].type << 8) | sub;
        }
    } else if (leaf == 0x16) {
        r[0] = CPU->base_mhz;
    } else if (leaf == 0x80000000u) {
        r[0] = CPU->max_ext;
    } else if (leaf == 0x80000001u) {
        r[2] = CPU->ext1_ecx;
    } else if (leaf >= 0x80000002u && leaf <= 0x80000004u) {
        char b[48];
        memset(b, 0, sizeof b);
        strncpy(b, CPU->brand, 47);
        int o = (int)(leaf - 0x80000002u) * 16;
        for (int i = 0; i < 4; i++)
            r[i] = word(b, o + i * 4);
    } else if (leaf == 0x80000008u) {
        r[2] = CPU->amd_08_ecx;
    } else if (leaf == 0x8000001Eu) {
        r[1] = CPU->amd_1e_ebx;
    }
}

static unsigned ids(unsigned *out, int n, unsigned first, unsigned step)
{
    for (int i = 0; i < n; i++)
        out[i] = first + (unsigned)i * step;
    return (unsigned)n;
}

/* ---- ACPI: RSDP -> XSDT -> MADT built by hand ---- */
static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(unsigned char *p, unsigned long long v) { put32(p, (unsigned)v); put32(p + 4, (unsigned)(v >> 32)); }
static void fix_sum(unsigned char *t, unsigned len, unsigned at)
{
    unsigned char s = 0;
    t[at] = 0;
    for (unsigned i = 0; i < len; i++)
        s = (unsigned char)(s + t[i]);
    t[at] = (unsigned char)(0 - s);
}

static void test_madt(void)
{
    static unsigned char madt[44 + 8 * 3 + 16 + 8], xsdt[36 + 8], rsdp[36];
    memset(madt, 0, sizeof madt); memset(xsdt, 0, sizeof xsdt); memset(rsdp, 0, sizeof rsdp);
    memcpy(madt, "APIC", 4);
    put32(madt + 4, sizeof madt);
    unsigned char *e = madt + 44;
    e[0] = 0; e[1] = 8; e[2] = 0; e[3] = 0; put32(e + 4, 1); e += 8;   /* APIC id 0, enabled */
    e[0] = 0; e[1] = 8; e[2] = 1; e[3] = 2; put32(e + 4, 1); e += 8;   /* APIC id 2, enabled */
    e[0] = 0; e[1] = 8; e[2] = 2; e[3] = 4; put32(e + 4, 0); e += 8;   /* APIC id 4, disabled: not counted */
    e[0] = 9; e[1] = 16; put32(e + 4, 0x100); put32(e + 8, 1); e += 16; /* x2APIC id 0x100, enabled */
    e[0] = 4; e[1] = 8; e += 8;                                         /* NMI entry: skipped by its length */
    fix_sum(madt, sizeof madt, 9);

    memcpy(xsdt, "XSDT", 4);
    put32(xsdt + 4, sizeof xsdt);
    put64(xsdt + 36, (unsigned long long)(size_t)madt);
    fix_sum(xsdt, sizeof xsdt, 9);

    memcpy(rsdp, "RSD PTR ", 8);
    rsdp[15] = 2;
    put32(rsdp + 20, 36);
    put64(rsdp + 24, (unsigned long long)(size_t)xsdt);
    unsigned char s = 0;
    for (int i = 0; i < 20; i++)
        s = (unsigned char)(s + rsdp[i]);
    rsdp[8] = (unsigned char)(0 - s);

    acpi_init((unsigned long long)(size_t)rsdp);
    CHECK(acpi_cpu_count() == 3);
    CHECK(acpi_cpu_apic_id(0) == 0 && acpi_cpu_apic_id(1) == 2 && acpi_cpu_apic_id(2) == 0x100);
    CHECK(acpi_cpu_apic_id(3) == 0 && acpi_cpu_apic_id(-1) == 0);

    madt[10] ^= 0xFF; /* a broken checksum: the table is ignored */
    acpi_init((unsigned long long)(size_t)rsdp);
    CHECK(acpi_cpu_count() == 0);
}

int main(void)
{
    unsigned a[256];
    const CpuInfo *c;

    /* Intel 4 cores x 2 threads (APIC ids 0..7), base speed from leaf 0x16 */
    Fake intel = {"GenuineIntel", "   Intel(R) Core(TM) i7-4770 CPU @ 3.40GHz", 0x16, 0x80000008u, 8u << 16, 1u << 28, 0, 3400,
                  2, {{1, 2, 1}, {4, 8, 2}}, 0, 0, 0};
    CPU = &intel;
    cpuinfo_probe(fake_cpuid, a, (int)ids(a, 8, 0, 1), 3000000);
    c = cpuinfo_get();
    CHECK(strcmp(c->vendor, "GenuineIntel") == 0);
    CHECK(strcmp(c->brand, "Intel(R) Core(TM) i7-4770 CPU @ 3.40GHz") == 0); /* leading blanks removed */
    CHECK(c->base_mhz == 3400 && !c->base_estimated);
    CHECK(c->sockets == 1 && c->cores == 4 && c->logical == 8);

    /* two sockets: second package starts at APIC id 16 */
    unsigned b[16];
    for (int i = 0; i < 8; i++) { b[i] = (unsigned)i; b[8 + i] = 16u + (unsigned)i; }
    cpuinfo_probe(fake_cpuid, b, 16, 3000000);
    CHECK(c->sockets == 2 && c->cores == 8 && c->logical == 16);

    /* hybrid: 2 P-cores with two threads (ids 0-3) + 4 E-cores with one (ids 8,10,12,14) */
    unsigned h[8] = {0, 1, 2, 3, 8, 10, 12, 14};
    cpuinfo_probe(fake_cpuid, h, 8, 3000000);
    CHECK(c->sockets == 1 && c->cores == 6 && c->logical == 8);

    /* a disabled-SMT or 1-thread-per-core CPU still counts */
    unsigned s4[4] = {0, 2, 4, 6};
    cpuinfo_probe(fake_cpuid, s4, 4, 3000000);
    CHECK(c->cores == 4 && c->logical == 4);

    /* AMD Zen: 8 cores x 2 threads, no leaf 0xB or 0x16: base speed falls back to the measured TSC */
    Fake amd = {"AuthenticAMD", "AMD Ryzen 7 5800X 8-Core Processor", 0x10, 0x8000001Fu, 16u << 16, 1u << 28, 0, 0,
                0, {{0, 0, 0}, {0, 0, 0}}, 1u << 22, 1u << 8, (4u << 12) | 15};
    CPU = &amd;
    cpuinfo_probe(fake_cpuid, a, (int)ids(a, 16, 0, 1), 3800000);
    CHECK(strcmp(c->vendor, "AuthenticAMD") == 0);
    CHECK(c->base_mhz == 3800 && c->base_estimated);
    CHECK(c->sockets == 1 && c->cores == 8 && c->logical == 16);

    /* old Intel, no MADT: leaf 1 says 4 logical, leaf 4 says 2 cores; base speed from the brand string */
    Fake old = {"GenuineIntel", "Intel(R) Core(TM)2 CPU 6600 @ 2.60GHz", 4, 0x80000008u, 4u << 16, 1u << 28, 1u << 26, 0,
                0, {{0, 0, 0}, {0, 0, 0}}, 0, 0, 0};
    CPU = &old;
    cpuinfo_probe(fake_cpuid, 0, 0, 0);
    CHECK(c->base_mhz == 2600 && !c->base_estimated);
    CHECK(c->sockets == 1 && c->cores == 2 && c->logical == 4);

    /* a plain 1-CPU machine (no HTT) with no brand string and no TSC: unknown speed, one of everything */
    Fake one = {"GenuineIntel", "", 1, 0, 1u << 16, 0, 0, 0, 0, {{0, 0, 0}, {0, 0, 0}}, 0, 0, 0};
    CPU = &one;
    cpuinfo_probe(fake_cpuid, 0, 0, 0);
    CHECK(c->base_mhz == 0 && c->brand[0] == 0);
    CHECK(c->sockets == 1 && c->cores == 1 && c->logical == 1);

    /* brand strings: MHz, a blank before the unit, no unit */
    Fake mhz = {"GenuineIntel", "Pentium III @ 800MHz", 1, 0x80000004u, 1u << 16, 0, 0, 0, 0, {{0, 0, 0}, {0, 0, 0}}, 0, 0, 0};
    CPU = &mhz;
    cpuinfo_probe(fake_cpuid, 0, 0, 0);
    CHECK(c->base_mhz == 800);
    Fake sp = {"GenuineIntel", "Xeon 2.5 GHz", 1, 0x80000004u, 1u << 16, 0, 0, 0, 0, {{0, 0, 0}, {0, 0, 0}}, 0, 0, 0};
    CPU = &sp;
    cpuinfo_probe(fake_cpuid, 0, 0, 0);
    CHECK(c->base_mhz == 2500);

    test_madt();

    if (fails == 0)
        printf("cpuinfo tests passed\n");
    return fails ? 1 : 0;
}
