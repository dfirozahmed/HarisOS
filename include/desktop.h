/* HarisOS desktop: wallpaper + welcome window + taskbar, drawn with HarisUI (ui.h).
 * Call gfx_init() first. */
#ifndef HARIS_DESKTOP_H
#define HARIS_DESKTOP_H

/* System info shown in the welcome window. */
void desktop_set_memory(unsigned long long free_mb, unsigned long long total_mb); /* total 0 = failed */
void desktop_set_double_buffered(int on);
void desktop_set_status(const char *line); /* optional extra line at the bottom (may be NULL) */

void desktop_draw(void);

#endif
