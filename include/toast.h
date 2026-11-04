/* HarisOS desktop toast: a short message that appears in the bottom right corner, above the taskbar, over the windows, and goes away
 * by itself after a few seconds (or when it is clicked). For the things an app wants to say that need no answer: "Saved",
 * "Not saved: the drive is locked", "Copied".
 *
 *   toast_show(TOAST_WARNING, "Icon places not saved - no writable drive.");
 *
 * One toast at a time: a new one replaces the one on the screen. The text is copied (up to TOAST_TEXT_MAX characters), so it can be
 * built on the stack. A long text goes on a second line. While the pointer is over a toast it stays. Any code may call toast_show;
 * apps use only this header. */
#ifndef HARIS_TOAST_H
#define HARIS_TOAST_H

#include "ui.h"

typedef enum
{
    TOAST_INFO,    /* accent colour: a fact */
    TOAST_SUCCESS, /* green: it worked */
    TOAST_WARNING, /* amber: it did not quite work, or something to know */
    TOAST_ERROR    /* red: it failed */
} ToastKind;

#define TOAST_TEXT_MAX 140

void toast_show(ToastKind kind, const char *text); /* NULL or an empty text: nothing */
void toast_dismiss(void);

/* ---- for the desktop (desktop_input.c, desktop_clock.c, the draw paths and the compositor), not for apps ---- */
int toast_is_open(void);
UiRect toast_bounds(void);                          /* everything it draws, its shadow included */
void toast_draw(void);                              /* over the windows, below the menus */
int toast_tick(void);                               /* on every main-loop pass: 1 when it went away by itself (draw again) */
int toast_pointer(int x, int y, unsigned buttons);  /* 1 while the pointer is over it; a left click on it dismisses it */

#endif
