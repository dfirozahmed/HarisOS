/* ACPI table walker. See include/acpi.h. */
#include "acpi.h"

#define MAX_AML 48
#define HEADER 36 /* size of every ACPI table header */
#define MAX_TABLE (8u << 20)

static const unsigned char *TAB[MAX_AML];
static unsigned LEN[MAX_AML];
static int NTAB;
static unsigned CPU_APIC[ACPI_MAX_CPUS];
static unsigned IOAPIC_ADDR[ACPI_MAX_IOAPICS], IOAPIC_GSI[ACPI_MAX_IOAPICS];
static int NIOAPIC;
static int NCPU;

static unsigned rd32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long rd64(const unsigned char *p) { return rd32(p) | ((unsigned long long)rd32(p + 4) << 32); }

static int sig_is(const unsigned char *t, const char *s)
{
    return t[0] == (unsigned char)s[0] && t[1] == (unsigned char)s[1] && t[2] == (unsigned char)s[2] &&
           t[3] == (unsigned char)s[3];
}

static int sum_ok(const unsigned char *t, unsigned n)
{
    unsigned char s = 0;
    for (unsigned i = 0; i < n; i++)
        s = (unsigned char)(s + t[i]);
    return s == 0;
}

/* A table pointer is only trusted if its header makes sense and its checksum adds up. */
static const unsigned char *table_at(unsigned long long addr, unsigned *len)
{
    if (!addr || addr >= 0x10000000000000ULL)
        return 0;
    const unsigned char *t = (const unsigned char *)addr;
    unsigned n = rd32(t + 4);
    if (n < HEADER || n > MAX_TABLE || !sum_ok(t, n))
        return 0;
    *len = n;
    return t;
}

static void add(unsigned long long addr)
{
    unsigned n;
    const unsigned char *t = table_at(addr, &n);
    if (t && NTAB < MAX_AML)
    {
        TAB[NTAB] = t;
        LEN[NTAB++] = n;
    }
}

/* MADT: after the 44-byte header come entries (type, length, ...). Type 0 = local APIC (8 bytes: APIC id at 3,
 * flags at 4), type 9 = x2APIC (16 bytes: APIC id at 4, flags at 8). Flags bit 0 = the processor is enabled.
 * Type 1 = I/O APIC (12 bytes: id at 2, MMIO address at 4, the first interrupt number it serves ("GSI base") at 8). */
static void read_madt(const unsigned char *t, unsigned n)
{
    for (unsigned off = 44; off + 2 <= n;)
    {
        unsigned type = t[off], len = t[off + 1];
        if (len < 2 || off + len > n)
            break;
        if (NCPU < ACPI_MAX_CPUS)
        {
            if (type == 0 && len >= 8 && (rd32(t + off + 4) & 1))
                CPU_APIC[NCPU++] = t[off + 3];
            else if (type == 9 && len >= 16 && (rd32(t + off + 8) & 1))
                CPU_APIC[NCPU++] = rd32(t + off + 4);
        }
        if (type == 1 && len >= 12 && NIOAPIC < ACPI_MAX_IOAPICS)
        {
            IOAPIC_ADDR[NIOAPIC] = rd32(t + off + 4);
            IOAPIC_GSI[NIOAPIC] = rd32(t + off + 8);
            NIOAPIC++;
        }
        off += len;
    }
}

void acpi_init(unsigned long long rsdp)
{
    NTAB = 0;
    NCPU = 0;
    NIOAPIC = 0;
    if (!rsdp)
        return;
    const unsigned char *r = (const unsigned char *)rsdp;
    if (r[0] != 'R' || r[1] != 'S' || r[2] != 'D' || r[3] != ' ' || r[4] != 'P' || r[5] != 'T' || r[6] != 'R' ||
        r[7] != ' ' || !sum_ok(r, 20))
        return;

    unsigned n;
    const unsigned char *root;
    int wide = 0;
    if (r[15] >= 2 && rd64(r + 24))
    {
        root = table_at(rd64(r + 24), &n); /* XSDT: 64-bit entries */
        wide = 1;
    }
    else
        root = table_at(rd32(r + 16), &n); /* RSDT: 32-bit entries */
    if (!root)
        return;

    unsigned step = wide ? 8 : 4;
    for (unsigned off = HEADER; off + step <= n; off += step)
    {
        unsigned long long a = wide ? rd64(root + off) : rd32(root + off);
        unsigned tl;
        const unsigned char *t = table_at(a, &tl);
        if (!t)
            continue;
        if (sig_is(t, "SSDT"))
            add(a);
        else if (sig_is(t, "APIC"))
            read_madt(t, tl);
        else if (sig_is(t, "FACP"))
        {
            unsigned long long dsdt = tl >= 148 ? rd64(t + 140) : 0; /* X_DSDT, else the 32-bit pointer */
            if (!dsdt)
                dsdt = rd32(t + 40);
            add(dsdt);
        }
    }
}

int acpi_cpu_count(void) { return NCPU; }

unsigned acpi_cpu_apic_id(int i) { return (i >= 0 && i < NCPU) ? CPU_APIC[i] : 0; }

int acpi_aml_count(void) { return NTAB; }

const unsigned char *acpi_aml(int i, unsigned *len)
{
    if (i < 0 || i >= NTAB)
        return 0;
    *len = LEN[i];
    return TAB[i];
}

int acpi_ioapic_count(void) { return NIOAPIC; }
unsigned acpi_ioapic_addr(int i) { return (i >= 0 && i < NIOAPIC) ? IOAPIC_ADDR[i] : 0; }
unsigned acpi_ioapic_gsi_base(int i) { return (i >= 0 && i < NIOAPIC) ? IOAPIC_GSI[i] : 0; }
