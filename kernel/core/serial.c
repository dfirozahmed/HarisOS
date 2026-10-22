/* HarisOS serial port (COM1) output: see include/serial.h. */
#include "serial.h"
#include "io.h"

enum { COM1 = 0x3F8 };

static int PRESENT;

void serial_init(void)
{
    /* The scratch register (COM1+7) holds what is written to it only if a UART is really there. Without this test a
     * PC with no serial port would make every character wait for a status bit that never comes. */
    io_out8(COM1 + 7, 0xAE);
    PRESENT = io_in8(COM1 + 7) == 0xAE;
    if (!PRESENT)
        return;
    io_out8(COM1 + 1, 0x00); /* no serial interrupts */
    io_out8(COM1 + 3, 0x80); /* DLAB on: set the baud rate divisor */
    io_out8(COM1 + 0, 0x01); /* 115200 baud */
    io_out8(COM1 + 1, 0x00);
    io_out8(COM1 + 3, 0x03); /* 8 bits, no parity, 1 stop bit */
    io_out8(COM1 + 2, 0xC7); /* FIFO on */
    io_out8(COM1 + 4, 0x0B);
}

int serial_present(void) { return PRESENT; }

void serial_putc(char c)
{
    if (!PRESENT)
        return;
    /* bounded wait: a stuck transmitter must never hang the crash screen */
    for (int i = 0; i < 100000 && !(io_in8(COM1 + 5) & 0x20); i++)
        __asm__ volatile("pause");
    io_out8(COM1, (unsigned char)c);
}

void serial_puts(const char *s)
{
    for (; *s; s++)
    {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s);
    }
}

void serial_putd(long long v)
{
    char tmp[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do
        tmp[n++] = (char)('0' + u % 10);
    while ((u /= 10) != 0);
    if (v < 0)
        serial_putc('-');
    while (n)
        serial_putc(tmp[--n]);
}
