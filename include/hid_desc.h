/* ============================================================
 * Just enough of the HID report descriptor to read a mouse.
 *
 * hid_find_mouse() looks for a Generic Desktop / Mouse collection with relative X and Y and
 * records where the buttons, X, Y and wheel sit inside its input report. hid_value() then
 * pulls a field out of a report.
 *
 * Pure functions, tested on the host (tests/test_i2chid.c).
 * ============================================================ */
#ifndef HARIS_HID_DESC_H
#define HARIS_HID_DESC_H

typedef struct
{
    int uses_ids;      /* the descriptor has report IDs: every report starts with an ID byte */
    int report_id;     /* ID of the mouse report (meaningful when uses_ids) */
    unsigned btn_off;  /* bit offset of the first button, counted after the ID byte */
    unsigned btn_n;    /* number of buttons */
    unsigned x_off, x_size, y_off, y_size; /* bit offset and width of relative X / Y */
    int x_signed, y_signed;
    int has_wheel;
    unsigned w_off, w_size;
    int w_signed;
} HidMouse;

/* Returns 1 and fills *m when the descriptor contains a usable mouse, else 0. */
int hid_find_mouse(const unsigned char *desc, unsigned len, HidMouse *m);

/* What the descriptor says about a Windows "precision touchpad" (Digitizers page, Touch Pad
 * application collection). Many laptop pads have this next to their mouse collection and start in
 * mouse mode; only the precision mode reports the fingers (needed for two-finger scrolling). */
typedef struct
{
    int is_ptp;         /* a Touch Pad application collection exists */
    int fingers;        /* Finger collections inside it = how many contacts one report carries */
    int has_mode;       /* a Feature item "Device Mode" exists: the way to switch the pad to precision mode */
    int mode_report_id; /* its report ID (0 = the descriptor has no IDs) */
} HidTouchpad;

/* Returns 1 and fills *t when a Touch Pad collection was found, else 0 (*t is still filled). */
int hid_find_touchpad(const unsigned char *desc, unsigned len, HidTouchpad *t);

/* Field of `size` bits (1..32) at bit offset `off` of data[0..len). Out-of-range bits read as 0. */
long hid_value(const unsigned char *data, unsigned len, unsigned off, unsigned size, int is_signed);

#endif
