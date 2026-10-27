/* I/O APIC. See include/ioapic.h.
 *
 * Each chip has two registers in memory: IOREGSEL (offset 0) takes the number of an internal register, IOWIN (offset 0x10) then
 * reads or writes it. Register 1 holds the version and the number of lines; line n has a 64-bit "redirection entry" in the
 * registers 0x10 + 2n (low half) and 0x11 + 2n (high half, whose top byte is the local APIC id the interrupt goes to). */
#include "ioapic.h"
#include "acpi.h"

#define MAX_CHIPS ACPI_MAX_IOAPICS
#define ENTRY_MASKED (1u << 16)

typedef struct
{
    volatile unsigned *regs;
    unsigned gsi_base, lines;
} Chip;

static Chip CHIPS[MAX_CHIPS];
static int NCHIPS;

unsigned ioapic_entry(unsigned vector, int level, int active_low, int masked)
{
    return (vector & 0xFFu) | (active_low ? 1u << 13 : 0) | (level ? 1u << 15 : 0) | (masked ? ENTRY_MASKED : 0);
}

static unsigned rd(const Chip *c, unsigned reg)
{
    c->regs[0] = reg;
    return c->regs[4];
}

static void wr(const Chip *c, unsigned reg, unsigned v)
{
    c->regs[0] = reg;
    c->regs[4] = v;
}

static const Chip *chip_of(unsigned gsi)
{
    for (int i = 0; i < NCHIPS; i++)
        if (gsi >= CHIPS[i].gsi_base && gsi < CHIPS[i].gsi_base + CHIPS[i].lines)
            return &CHIPS[i];
    return 0;
}

void ioapic_init(void)
{
    NCHIPS = 0;
    int n = acpi_ioapic_count();
    for (int i = 0; i < (n ? n : 1) && NCHIPS < MAX_CHIPS; i++)
    {
        Chip *c = &CHIPS[NCHIPS];
        c->regs = (volatile unsigned *)(unsigned long long)(n ? acpi_ioapic_addr(i) : IOAPIC_DEFAULT_ADDR);
        c->gsi_base = n ? acpi_ioapic_gsi_base(i) : 0;
        unsigned ver = c->regs ? rd(c, 1) : 0;
        if (ver == 0xFFFFFFFFu || ver == 0) /* nothing answers at that address */
            continue;
        c->lines = ((ver >> 16) & 0xFF) + 1;
        for (unsigned l = 0; l < c->lines; l++) /* every line silent until a driver asks */
            wr(c, 0x10 + 2 * l, rd(c, 0x10 + 2 * l) | ENTRY_MASKED);
        NCHIPS++;
    }
}

int ioapic_present(void) { return NCHIPS > 0; }

unsigned ioapic_gsi_count(void)
{
    unsigned top = 0;
    for (int i = 0; i < NCHIPS; i++)
        if (CHIPS[i].gsi_base + CHIPS[i].lines > top)
            top = CHIPS[i].gsi_base + CHIPS[i].lines;
    return top;
}

int ioapic_route(unsigned gsi, unsigned vector, unsigned apic_id)
{
    const Chip *c = chip_of(gsi);
    if (!c || vector < 0x20 || vector > 0xFE || apic_id > 0xFF)
        return -1;
    unsigned line = gsi - c->gsi_base;
    wr(c, 0x11 + 2 * line, apic_id << 24);                         /* destination first, while the line is still masked */
    wr(c, 0x10 + 2 * line, ioapic_entry(vector, 1, 1, 0));
    return 0;
}

void ioapic_mask(unsigned gsi)
{
    const Chip *c = chip_of(gsi);
    if (c)
        wr(c, 0x10 + 2 * (gsi - c->gsi_base), rd(c, 0x10 + 2 * (gsi - c->gsi_base)) | ENTRY_MASKED);
}
