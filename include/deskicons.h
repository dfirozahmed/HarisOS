/* HarisOS desktop icons, on the wallpaper below the windows. Two kinds:
 *
 *   an app shortcut, for every app of the registry (app.h). They are all there at first. Delete removes one from the desktop (the app
 *   itself stays in the Start menu); "Send to desktop" in the Start menu brings it back.
 *   a file or folder of the Desktop folder (deskfiles.h), the way Windows shows its desktop.
 *
 *   Click    selects an icon, a click on the bare desktop selects none.
 *   Double click (or the menu's Open, or Enter) opens it.
 *   Drag     moves an icon; it snaps to the nearest cell of the grid on release. If that cell has an icon, the two swap places.
 *   Delete   (the menu, or the Delete key when no window has the keyboard): an app shortcut goes at once, a file asks first.
 *
 * Where each icon is stays in ICONS.CFG, next to THEME.CFG, read by deskicons_load() at boot and written after every change. One line
 * each: "File Explorer=0,1" (column 0, row 1), "Notepad=hidden" (removed from the desktop), "file:notes.txt=3,2" (a file).
 *
 * The desktop (desktop_input.c) feeds the pointer in, draws the icons after the wallpaper and before the windows
 * (desktop_draw, desktop_draw_region), and, with the GPU compositor, keeps them in a layer of their own (desktop_comp.c). */
#ifndef HARIS_DESKICONS_H
#define HARIS_DESKICONS_H

#include "keyboard.h"
#include "ui.h"

#define DESK_FILE_NAME 96 /* VFS_NAME_MAX */
#define DESK_FILES_MAX 48 /* files and folders of the Desktop folder that get an icon (the first ones, folders first) */

/* One entry of the Desktop folder, as deskfiles.c tells it. */
typedef struct
{
    int node;      /* its vfs node */
    int is_dir;
    char name[DESK_FILE_NAME];
} DeskFile;

void deskicons_load(void); /* at boot, after the drives are mounted: the places of ICONS.CFG (none: the default order) */

/* What is in the Desktop folder now (every second, and when it changes). found = 0: there is no Desktop folder (the places of its files
 * that ICONS.CFG knows are kept for when it comes back). */
void deskicons_set_files(const DeskFile *files, int n, int found);

/* The pointer, on every mouse event. on_desktop = it is on the bare desktop (no window, menu or taskbar under it, no window being
 * dragged): only then a click starts something. Returns 1 while the icons own the event (a press on an icon, or a drag going on),
 * so the windows must not see it. */
int deskicons_pointer(int x, int y, unsigned buttons, int on_desktop);

/* The Delete and Enter keys, for when no window has the keyboard (the desktop calls this only then). 1 = used. */
int deskicons_key(const KeyEvent *k);

int deskicons_at(int x, int y);  /* the icon under (x, y), -1 = none */
void deskicons_select(int icon); /* -1 = none */
int deskicons_is_file(int icon); /* 1 = a file or folder of the Desktop folder, 0 = an app shortcut (or no such icon) */
void deskicons_open(int icon);   /* open it */
void deskicons_delete(int icon); /* an app shortcut goes from the desktop; a file asks (deskfiles_ask_delete) */
void deskicons_arrange(void);    /* back to the default order (and saved) */

/* "Send to desktop": app `app` (an index of the registry) gets its shortcut back on the desktop and is selected. A toast says what happened. */
void deskicons_send_app(int app);

void deskicons_draw(void);        /* clipped to the current clip */
int deskicons_visible(void);      /* there are icons to draw */
UiRect deskicons_bounds(void);    /* everything they draw: the grid cells in use */

/* An icon that is being dragged is not in the grid layer: it follows the pointer above the windows (and the outline of the cell it
 * would land in), as a layer of its own that is there only during the drag. */
int deskicons_dragging(void);
UiRect deskicons_ghost_bounds(void); /* the whole desktop above the taskbar */
void deskicons_ghost_draw(void);

#endif
