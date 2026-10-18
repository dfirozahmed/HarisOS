/* ============================================================
 * PS/2 controller (i8042): the keyboard and the mouse share one data port, so
 * one small module reads it and sorts the bytes into two queues.
 *
 *   ps2_poll()          read what the controller has (never blocks)
 *   ps2_kbd_pop(&b)     next keyboard byte, 1 = got one
 *   ps2_mouse_pop(&b)   next mouse byte,    1 = got one
 *
 * mouse.c and keyboard.c both call ps2_poll() first, so neither one steals
 * the other's bytes.
 * ============================================================ */
#ifndef HARIS_PS2_H
#define HARIS_PS2_H

void ps2_poll(void);
int ps2_kbd_pop(unsigned char *b);
int ps2_mouse_pop(unsigned char *b);

#endif
