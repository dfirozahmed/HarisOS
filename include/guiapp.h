/* ============================================================
 * HarisOS compositor: shows the surfaces of user programs (gui.h) as desktop windows
 *
 *   gui_app_poll();   once per main-loop pass: opens a window for every new surface, closes the window of every surface that is gone.
 *                     Returns 1 if a window was opened or closed (then redraw).
 *
 * Everything else happens in the window's own callbacks (apps/gui): it copies the client's pixels to the screen when they were presented,
 * and turns the keys and mouse of the window into events for the client. System calls never touch windows - they only set flags in
 * gui.c, which this notices on its next pass (the same way the console window notices output).
 * ============================================================ */
#ifndef HARIS_GUIAPP_H
#define HARIS_GUIAPP_H

int gui_app_poll(void);

#endif
