/* HarisOS diagnostic trace: see include/dlog.h. */
#include "dlog.h"
#include "exec.h"
#include "serial.h"
#include "timer.h"
#include "vfs.h"

#define DLOG_MAX 24000
#define DLOG_NAME "TRACE.TXT"

static char BUF[DLOG_MAX];
static unsigned LEN;
static volatile int PENDING;
static unsigned long long SAVE_AT; /* 0 = no timed save armed */
static int ARMED_ONCE;

static void put_char(char c)
{
    if (LEN < DLOG_MAX - 1)
        BUF[LEN++] = c;
}

static void put_str(const char *s)
{
    for (; s && *s; s++)
        put_char(*s);
}

static void put_num(long long v)
{
    char t[24];
    int n = 0;
    unsigned long long u;
    if (v < 0)
    {
        put_char('-');
        u = (unsigned long long)(-v);
    }
    else
        u = (unsigned long long)v;
    if (u == 0)
        t[n++] = '0';
    while (u)
    {
        t[n++] = (char)('0' + u % 10);
        u /= 10;
    }
    while (n)
        put_char(t[--n]);
}

void dlog(const char *tag, const char *who, long long a, long long b, long long c)
{
    if (LEN > DLOG_MAX - 120) /* full: the oldest half goes (from a line start), the latest events are what matters */
    {
        unsigned cut = DLOG_MAX / 2;
        while (cut < LEN && BUF[cut - 1] != '\n')
            cut++;
        for (unsigned i = cut; i < LEN; i++)
            BUF[i - cut] = BUF[i];
        LEN -= cut;
    }
    put_char('[');
    put_num((long long)timer_ms());
    put_str("] ");
    put_str(tag);
    if (who)
    {
        put_char(' ');
        put_str(who);
    }
    put_char(' ');
    put_num(a);
    put_char(' ');
    put_num(b);
    put_char(' ');
    put_num(c);
    put_char('\n');
    BUF[LEN] = 0;
}

void dlog_arm_save(unsigned ms)
{
    if (ARMED_ONCE)
        return;
    ARMED_ONCE = 1;
    SAVE_AT = timer_ms() + ms;
}

void dlog_request_save(void) { PENDING = 1; }

int dlog_save_due(void)
{
    if (PENDING)
        return 1;
    return SAVE_AT && timer_ms() >= SAVE_AT;
}

static const char *NOTE = "TRACE.TXT";
const char *dlog_save_note(void) { return NOTE; }

int dlog_save(void)
{
    PENDING = 0;
    SAVE_AT = 0;
    unsigned n = LEN; /* lines added while the file is written stay for the next time */
    int k = exec_find("KERNEL.EFI");
    if (k < 0)
        return 0;
    int vol = vfs_volume_of(k);
    int dir = vfs_volume_node(vol);
    VfsVolInfo vi;
    if (dir < 0 || vfs_volume_info(vol, &vi) != 0)
        return 0;
    int was_locked = vi.locked;
    if (was_locked && vfs_set_locked(vol, 0) != 0)
        return 0;
    int ok = vfs_save(dir, DLOG_NAME, BUF, n) >= 0;
    if (was_locked)
        vfs_set_locked(vol, 1);
    serial_puts(ok ? "dlog: TRACE.TXT written\n" : "dlog: TRACE.TXT could not be written\n");
    NOTE = ok ? "Trace saved to TRACE.TXT" : "Trace could not be saved (is the boot drive writable?)";
    return ok;
}
