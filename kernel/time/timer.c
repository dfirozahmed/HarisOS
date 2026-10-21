/* HarisOS timer: see include/timer.h. */
#include "timer.h"

#if defined(__x86_64__) || defined(__i386__)

#define PIT_HZ 1193182ULL

static unsigned long long TSC_BASE, TSC_PER_MS;

static void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static unsigned long long rdtsc(void)
{
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* current value of PIT channel 0 (counts down, 16 bit) */
static unsigned pit_read(void)
{
    outb(0x43, 0x00); /* latch channel 0 */
    unsigned lo = inb(0x40);
    unsigned hi = inb(0x40);
    return lo | (hi << 8);
}

void timer_init(void)
{
    /* channel 0, lobyte/hibyte, mode 2 (rate generator), binary, reload 65536 */
    outb(0x43, 0x34);
    outb(0x40, 0);
    outb(0x40, 0);

    const unsigned want = 40000; /* PIT ticks to measure: about 33.5 ms */
    unsigned elapsed = 0, prev = pit_read();
    unsigned long long t0 = rdtsc();
    for (long i = 0; i < 300000 && elapsed < want; i++)
    {
        unsigned now = pit_read();
        elapsed += (prev - now) & 0xFFFF; /* the counter counts down and wraps at 16 bit */
        prev = now;
    }
    unsigned long long t1 = rdtsc();
    if (elapsed < want || t1 <= t0)
        return; /* no PIT: stay "not ready" */

    /* cycles per millisecond = cycles * PIT_HZ / (ticks * 1000) */
    TSC_PER_MS = (t1 - t0) * PIT_HZ / ((unsigned long long)elapsed * 1000ULL);
    TSC_BASE = t1;
}

int timer_ready(void) { return TSC_PER_MS != 0; }

unsigned long long timer_ms(void)
{
    if (!TSC_PER_MS)
        return 0;
    return (rdtsc() - TSC_BASE) / TSC_PER_MS;
}

unsigned timer_tsc_khz(void) { return (unsigned)TSC_PER_MS; } /* cycles per millisecond = kHz */

unsigned long long timer_us(void)
{
    if (!TSC_PER_MS)
        return 0;
    return (rdtsc() - TSC_BASE) * 1000ULL / TSC_PER_MS;
}

#else

void timer_init(void) {}
int timer_ready(void) { return 0; }
unsigned long long timer_ms(void) { return 0; }
unsigned timer_tsc_khz(void) { return 0; }
unsigned long long timer_us(void) { return 0; }

#endif
