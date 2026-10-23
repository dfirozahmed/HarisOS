/* ============================================================
 * HarisOS GUI surfaces: the compositor side of the protocol a ring 3 program uses to get a window
 *
 *   A client (user program) asks for a surface with SYS_GUI_CREATE. The kernel makes a shared memory segment for its pixels and
 *   maps it into the client; the kernel's window manager shows that memory in an ordinary desktop window (apps/gui). The client
 *   draws into its own copy of the memory, then tells the compositor which part changed (SYS_GUI_PRESENT). Mouse and keyboard
 *   events for the window travel the other way, through a queue the client reads with SYS_GUI_EVENT. See syscall.h for the calls.
 *
 *   int h = gui_create("Title", w, h, pid, &seg);   a surface owned by process `pid`; its pixels are shm segment `seg`
 *   gui_present(h, x, y, w, h);                      client: this part of the pixels changed (w = 0: all of it)
 *   gui_post(h, &ev);                                compositor: queue an event for the client (a mouse move replaces a waiting one)
 *   gui_next_event(h, &ev);                          client: take the oldest event (the system call sleeps in gui_wait when none)
 *   gui_destroy(h);                                  either side is done; gui_release_owner(pid) when a process ends
 *
 * Pixels: 32 bits each, 0x00RRGGBB, row after row, no padding between rows (a row is width * 4 bytes). A surface is GUI_W_MIN ..
 * GUI_W_MAX wide and GUI_H_MIN .. GUI_H_MAX high (the segment is at most 1 MiB).
 *
 * A surface is named by a handle: slot and generation, like a console (console.h), so a stale handle is simply invalid. Every
 * function accepts an invalid handle and does nothing / says so. gui.c has no window code: it is a table, a queue and some
 * flags, tested on the host (tests/test_gui.c). The window is apps/gui, which polls it from the main loop - no window code
 * ever runs inside a system call.
 * ============================================================ */
#ifndef HARIS_GUI_H
#define HARIS_GUI_H

#define GUI_MAX 4
#define GUI_TITLE_MAX 32
#define GUI_W_MIN 64
#define GUI_H_MIN 48
#define GUI_W_MAX 512
#define GUI_H_MAX 512
#define GUI_EVENTS 32 /* events kept per surface; when the client is too slow the newest are dropped (not a close request) */

/* Event types (GuiEvent.type). Positions are in the surface's own pixels, (0,0) = its top-left corner. */
#define GUI_EV_KEY 1        /* key, ch, pressed, mods as in keyboard.h (key is KEY_* or the lowercase character) */
#define GUI_EV_MOUSE_MOVE 2 /* x, y, buttons (bit 0 = left, bit 1 = right) */
#define GUI_EV_MOUSE_DOWN 3 /* x, y, buttons = the button that went down only */
#define GUI_EV_MOUSE_UP 4   /* x, y; the left button went up (buttons = 0). The right button reports only its press. */
#define GUI_EV_WHEEL 5      /* x = steps, + = down, like MouseState.wheel */
#define GUI_EV_CLOSE 6      /* the user asked to close the window: the client should clean up and SYS_GUI_CTL destroy it */

/* What SYS_GUI_EVENT stores (the same layout in user/sys.h). Fields a type does not use are 0. */
typedef struct
{
    int type;
    int x, y;
    int buttons;
    int key, ch, pressed, mods;
} GuiEvent;

/* ---- client side ------------------------------------------------ */
/* A new surface of w x h pixels, owned by process pid. Returns its handle (never 0 or negative), or -1: size out of range, a
 * bad title, no free slot, no memory. *seg = the shm segment (shm.h) holding the pixels: the caller maps it into the client; the
 * surface holds one reference of its own, so the segment lives until gui_destroy and the client's unmapping. */
int gui_create(const char *title, int w, int h, int pid, int *seg);
void gui_destroy(int hnd);       /* the surface is gone: a client waiting for events is woken and sees -1 */
void gui_release_owner(int pid); /* gui_destroy for every surface of that process */
int gui_valid(int hnd);
int gui_owner(int hnd);          /* pid, -1 for an invalid handle */
int gui_size(int hnd, int *w, int *h); /* 1, or 0 for an invalid handle */
const char *gui_title(int hnd);
int gui_set_title(int hnd, const char *title); /* 0, or -1 (invalid handle, empty or too long title) */

/* The part of the pixels that changed: clipped to the surface, joined with what is already waiting. 0, or -1 (invalid). */
int gui_present(int hnd, int x, int y, int w, int h);

/* The oldest event: 1 and *out filled, 0 = none, -1 = invalid handle (the surface is gone). */
int gui_next_event(int hnd, GuiEvent *out);
void gui_wait(int hnd, int task_id); /* wake task_id (task_wake) when an event arrives or the surface is destroyed */

/* ---- compositor side -------------------------------------------- */
/* Queue an event. 1 = queued, 0 = dropped (queue full), -1 = invalid. A MOUSE_MOVE that follows a waiting MOUSE_MOVE replaces it;
 * GUI_EV_CLOSE always finds room. */
int gui_post(int hnd, const GuiEvent *ev);

/* The user asked to close the window. 1 = a GUI_EV_CLOSE was queued: keep the window open, the client will destroy the surface.
 * 0 = close it now: the client has not read the previous request (it is stuck), or the handle is invalid. */
int gui_request_close(int hnd);

int gui_handle_at(int slot);                      /* the handle of the surface in that slot, -1 if none */
int gui_take_dirty(int hnd, int *x, int *y, int *w, int *h); /* 1 and the changed rectangle (once), 0 = nothing new */
int gui_take_title_changed(int hnd);              /* 1 once after every gui_set_title */

/* The pixels from (x, y) to the end of their row or the end of their page (the segment's pages are not next to each other),
 * whichever is first: *n = how many. NULL for a position outside the surface. */
const unsigned *gui_pixels(int hnd, int x, int y, int *n);

int gui_in_use(void); /* surfaces that exist, for leak checks */

#endif
