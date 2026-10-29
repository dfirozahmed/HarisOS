#ifndef HARISOS_DESKTOP_INTERNAL_H
#define HARISOS_DESKTOP_INTERNAL_H
#include "wm.h"
extern const char *STATUS_LINE;
extern unsigned long long MEM_FREE_MB, MEM_TOTAL_MB;
extern int DOUBLE_BUFFERED, MOUSE_PRESENT;
extern Window *WELCOME;
extern int DARK, WALL_VALID;
char *desktop_put_str(char *out, const char *s);
char *desktop_put_num(char *out, unsigned long long value);
void welcome_create(void);
void open_hello_window(void);
Window *desktop_taskbar_window(int i);
void desktop_toggle_theme(void);
void desktop_power_action(int restart); /* full-screen message, then shut down (0) or restart (1); returns only if a window has unsaved work and asked the user first (nothing is shut down) */ /* dark <-> light */
int desktop_refresh_taskbar(void);
int desktop_finish_input(int bar_changed);
/* desktop_comp.c: this frame's changed areas through the GPU compositor; 1 = done, 0 = draw them with the CPU */
int desktop_comp_frame(const UiRect *areas, int n);
int desktop_comp_pointer_pending(void); /* the GPU draws the pointer and it moved: a frame is due even without other changes */

#endif
