/* ============================================================
 * HarisOS keyboard driver (PS/2, polled, US layout)
 *
 *   keyboard_init();               once
 *   KeyEvent e;
 *   while (keyboard_poll(&e)) {...}   call often; one event per key press / release
 *
 * An event has:
 *   key      which key: a printable key is its lowercase character ('a', '1', ' ', ';'),
 *            other keys use the KEY_* values below
 *   ch       the character to type (Shift and Caps Lock applied), 0 if the key types nothing
 *            ('\n' for Enter, '\t' for Tab)
 *   pressed  1 = key went down (also repeats while held), 0 = key released
 *   mods     KMOD_* bits held at that moment
 *
 * Works with PS/2 keyboards and, through the xHCI driver (usb.h), with USB keyboards
 * (HID boot protocol, plugged straight into the machine, no hub). USB keys repeat in
 * software (after 500 ms, then about 30 times a second).
 * ============================================================ */
#ifndef HARIS_KEYBOARD_H
#define HARIS_KEYBOARD_H

#define KMOD_SHIFT 1
#define KMOD_CTRL 2
#define KMOD_ALT 4
#define KMOD_CAPS 8 /* Caps Lock is on */

enum
{
    KEY_NONE = 0,
    KEY_ESC = 256,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ENTER,
    KEY_DELETE,
    KEY_INSERT,
    KEY_HOME,
    KEY_END,
    KEY_PAGEUP,
    KEY_PAGEDOWN,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_SHIFT,
    KEY_CTRL,
    KEY_ALT,
    KEY_CAPS,
    KEY_SUPER, /* Windows key */
    KEY_F1 = 288, /* KEY_F1 .. KEY_F12 are consecutive */
    KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12
};

typedef struct
{
    int key;
    int ch;
    int pressed;
    unsigned mods;
} KeyEvent;

/* Returns 0 when a keyboard answered, -1 otherwise (polling still works). */
int keyboard_init(void);

/* Returns 1 and fills *out if there is an event, 0 if not. Never blocks. */
int keyboard_poll(KeyEvent *out);

#endif
