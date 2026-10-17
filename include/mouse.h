/* ============================================================
 * HarisOS mouse driver (PS/2, polled)
 *
 *   mouse_init(width, height)   once, after the screen size is known
 *   mouse_poll(&m)              call often (e.g. every loop pass)
 *
 * ============================================================ */
#ifndef HARIS_MOUSE_H
#define HARIS_MOUSE_H

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4

typedef struct
{
    int x, y;            /* pointer position in pixels */
    int dx, dy;          /* movement since the previous poll */
    int wheel;           /* wheel steps since the previous poll (+ = down) */
    unsigned buttons;    /* MOUSE_* bits currently held */
} MouseState;

/* Returns 0 when a mouse answered, -1 when there is none. */
int mouse_init(int screen_w, int screen_h);

/* Returns 1 if anything changed (movement, wheel or buttons), else 0. */
int mouse_poll(MouseState *out);

int mouse_present(void);

#endif
