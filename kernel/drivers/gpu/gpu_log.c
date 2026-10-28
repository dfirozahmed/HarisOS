#include "gpu_log.h"
#include "serial.h"

/* Everything the AMD driver prints is also kept here (16 KiB), so it can be saved to a file (a laptop has no COM1 to watch). */
#define LOG_MAX 16384
static char log_buf[LOG_MAX];
static unsigned log_len;
static int log_cut;

static void log_add(const char *s)
{
    while (*s)
    {
        char c = *s++;
        if (c == '\r')
            continue; /* the file uses plain \n */
        if (log_len + 1 >= LOG_MAX)
        {
            log_cut = 1;
            return;
        }
        log_buf[log_len++] = c;
    }
}

void serial_print(const char *s)
{
    serial_puts(s);
    log_add(s);
}

void serial_hex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    for (int i = 0; i < 8; i++)
        buf[i] = hex[(value >> (28 - 4 * i)) & 0xF];
    buf[8] = 0;
    serial_puts(buf);
    log_add(buf);
}

void serial_dec32(uint32_t value)
{
    char buf[11];
    int n = 0;
    do
    {
        buf[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    char out[11];
    for (int i = 0; i < n; i++)
        out[i] = buf[n - 1 - i];
    out[n] = 0;
    serial_puts(out);
    log_add(out);
}

const char *gpu_log_text(unsigned *len)
{
    if (len)
        *len = log_len;
    return log_buf;
}

int gpu_log_truncated(void)
{
    return log_cut;
}
