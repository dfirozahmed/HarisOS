/* ============================================================
 * HarisOS boot splash: a full-screen "Welcome to HarisOS" scene with a
 * progress bar, shown while the kernel starts up. The desktop's first
 * frame draws over it.
 *
 *   splash_show();                      draw the scene (kernel_main does it right after gfx_init)
 *   splash_progress(40, "Starting ...");  move the bar (0..100) and change the line under it
 *
 * Both do nothing while there is no screen (gfx_ready() == 0).
 * ============================================================ */
#ifndef HARIS_BOOTSPLASH_H
#define HARIS_BOOTSPLASH_H

void splash_show(void);
void splash_progress(int percent, const char *status);

#endif
