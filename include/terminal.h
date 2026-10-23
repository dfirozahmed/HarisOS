/* ============================================================
 * HarisOS terminal: a console window with a program running in it
 *
 *   terminal_open("SH.ELF");     a new Console window (console.h) and the program in it; 0, or -1 if no window / console
 *                                is free. If the program cannot be started the window opens anyway and says why.
 *
 * The Start menu's "Terminal" app opens SH.ELF this way, and the kernel starts INIT.ELF this way at the end of boot (if the
 * file exists): init then runs the shell in that window and starts it again when the user types "exit". Call it from the
 * main loop or a UI handler (it opens a window), never from a system call.
 * ============================================================ */
#ifndef HARIS_TERMINAL_H
#define HARIS_TERMINAL_H

int terminal_open(const char *program);

#endif
