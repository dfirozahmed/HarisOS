/* ============================================================
 * HarisOS serial port (COM1) output, for debugging
 *
 *   serial_init();                 once (calling it again is fine); finds out whether a UART is there
 *   serial_puts("text\n");         '\n' is sent as CR LF; nothing happens when there is no UART
 *   serial_putd(-12);              a number in decimal
 *
 * QEMU shows it with "-serial stdio" (or "-serial file:log.txt"). Used by the crash screen (fault.c) and for the
 * boot self test result.
 * ============================================================ */
#ifndef HARIS_SERIAL_H
#define HARIS_SERIAL_H

void serial_init(void);
int serial_present(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_putd(long long v);

#endif
