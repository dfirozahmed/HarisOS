#include "gpu_log.h"
#include "serial.h"

void serial_print(const char *s)
{
    serial_puts(s);
}

void serial_hex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    for (int i = 0; i < 8; i++)
        buf[i] = hex[(value >> (28 - 4 * i)) & 0xF];
    buf[8] = 0;
    serial_puts(buf);
}

void serial_dec32(uint32_t value)
{
    serial_putd((long long)value);
}
