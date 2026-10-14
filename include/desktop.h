#ifndef HARIS_DESKTOP_H
#define HARIS_DESKTOP_H

/* The desktop draws through the gfx layer: call gfx_init() first. */
void desktop_set_status(const char *line); /* one line shown in the welcome window */
void desktop_draw(void);

#endif
