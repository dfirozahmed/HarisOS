/* ============================================================
 * HarisOS diagnostic trace: a small text log in memory that is written to TRACE.TXT in the top folder of the boot drive.
 *
 * A machine without a serial port (a laptop) cannot show the serial lines the kernel prints. The places that measure how long
 * something takes (a drive read, a system call, a gap of the main loop, the first picture of a window) put a line here as well. The
 * file is written once, a while after a demand paged program started (dlog_arm_save), and again when it ended (dlog_request_save);
 * the main loop does the writing (dlog_save_due / dlog_save), because only it may use the drivers.
 *
 *   dlog("vfs_read", 0, offset, length, ms);   ->   [12345] vfs_read  67584 32768 41
 * ============================================================ */
#ifndef HARIS_DLOG_H
#define HARIS_DLOG_H

/* One line: "[time in ms] tag who a b c". `who` may be NULL. The log holds about 24 KB; when it is full nothing more is added. */
void dlog(const char *tag, const char *who, long long a, long long b, long long c);

/* The file is written the first time the main loop asks dlog_save_due() after `ms` milliseconds. Only the first call counts. */
void dlog_arm_save(unsigned ms);
/* Write the file at the next dlog_save_due(). */
void dlog_request_save(void);
/* 1 when the file should be written now (the main loop calls dlog_save then). */
int dlog_save_due(void);
/* Writes TRACE.TXT (the drive holding KERNEL.EFI is unlocked for it and locked again). 1 = written, 0 = not. */
int dlog_save(void);
/* A short sentence about the last dlog_save() (for a toast). */
const char *dlog_save_note(void);

#endif
