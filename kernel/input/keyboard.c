/* PS/2 keyboard: scan code set 1 (the controller translates set 2 into it) -> KeyEvent. */
#include "keyboard.h"
#include "io.h"
#include "ps2.h"

/* ---- key tables (scan code set 1, index = make code) ---------- */
#define TABLE_SIZE 0x59

/* character without Shift / with Shift; 0 = not a typing key */
static const char NORMAL[TABLE_SIZE] = {
    /* 00 */ 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, '\t',
    /* 10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    /* 20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    /* 30 */ 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    /* 50 */ '2', '3', '0', '.', 0, 0, 0, 0, 0,
};
static const char SHIFTED[TABLE_SIZE] = {
    /* 00 */ 0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, '\t',
    /* 10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    /* 20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    /* 30 */ 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    /* 50 */ '2', '3', '0', '.', 0, 0, 0, 0, 0,
};

static int is_letter(int c) { return c >= 'a' && c <= 'z'; }

/* the key for a make code that is not a plain typing key (no E0 prefix) */
static int special_key(int code)
{
    switch (code)
    {
    case 0x01: return KEY_ESC;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    case 0x1D: return KEY_CTRL;
    case 0x2A: case 0x36: return KEY_SHIFT;
    case 0x38: return KEY_ALT;
    case 0x3A: return KEY_CAPS;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F11 + 1; /* F12 */
    }
    if (code >= 0x3B && code <= 0x44)
        return KEY_F1 + (code - 0x3B);
    return KEY_NONE;
}

/* the key for an E0-prefixed make code */
static int extended_key(int code)
{
    switch (code)
    {
    case 0x1C: return KEY_ENTER; /* keypad enter */
    case 0x1D: return KEY_CTRL;  /* right Ctrl */
    case 0x38: return KEY_ALT;   /* right Alt (AltGr) */
    case 0x35: return '/';       /* keypad / */
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PAGEUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGEDOWN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5B: case 0x5C: return KEY_SUPER;
    }
    return KEY_NONE;
}

/* ---- decoder state ------------------------------------------- */
static unsigned MODS;       /* KMOD_* currently held */
static int SHIFT_L, SHIFT_R; /* each Shift key separately */
static int CTRL_L, CTRL_R, ALT_L, ALT_R;
static int CAPS_HELD;       /* Caps Lock is physically down (a held key repeats) */
static int E0;              /* the previous byte was 0xE0 */
static int SKIP;            /* bytes still to ignore (the Pause key sends six) */

static void set_mod(unsigned bit, int on)
{
    if (on)
        MODS |= bit;
    else
        MODS &= ~bit;
}

int keyboard_poll(KeyEvent *out)
{
    ps2_poll();
    unsigned char b;
    while (ps2_kbd_pop(&b))
    {
        if (SKIP)
        {
            SKIP--;
            continue;
        }
        if (b == 0xE1) /* Pause/Break: E1 1D 45 E1 9D C5 - ignore the rest */
        {
            SKIP = 5;
            continue;
        }
        if (b == 0xE0)
        {
            E0 = 1;
            continue;
        }
        int ext = E0;
        E0 = 0;

        int pressed = !(b & 0x80);
        int code = b & 0x7F;
        if (ext && (code == 0x2A || code == 0x36)) /* fake Shift around Print Screen / Insert / ... */
            continue;
        if (b == 0xFA || b == 0xFE || b == 0x00 || b == 0xFF) /* ack / resend / buffer overrun */
            continue;

        int key = ext ? extended_key(code) : special_key(code);
        if (!key && !ext && code < TABLE_SIZE && NORMAL[code])
            key = NORMAL[code];
        if (!key)
            continue;

        /* modifier keys: remember which physical key is down (left and right separately) */
        switch (key)
        {
        case KEY_SHIFT:
            if (code == 0x2A) SHIFT_L = pressed; else SHIFT_R = pressed;
            set_mod(KMOD_SHIFT, SHIFT_L || SHIFT_R);
            break;
        case KEY_CTRL:
            if (ext) CTRL_R = pressed; else CTRL_L = pressed;
            set_mod(KMOD_CTRL, CTRL_L || CTRL_R);
            break;
        case KEY_ALT:
            if (ext) ALT_R = pressed; else ALT_L = pressed;
            set_mod(KMOD_ALT, ALT_L || ALT_R);
            break;
        case KEY_CAPS:
            if (pressed && !CAPS_HELD) /* toggles once per press, not on every repeat */
                MODS ^= KMOD_CAPS;
            CAPS_HELD = pressed;
            break;
        }

        /* the character this key types */
        int ch = 0;
        if (!ext && code < TABLE_SIZE && NORMAL[code])
        {
            int shift = (MODS & KMOD_SHIFT) != 0;
            if (is_letter(NORMAL[code]) && (MODS & KMOD_CAPS))
                shift = !shift;
            ch = shift ? SHIFTED[code] : NORMAL[code];
        }
        else if (ext && key == '/')
            ch = '/';
        else if (ext && key == KEY_ENTER)
            ch = '\n';
        if (MODS & (KMOD_CTRL | KMOD_ALT))
            ch = 0; /* shortcuts, not typing */

        out->key = key;
        out->ch = ch;
        out->pressed = pressed;
        out->mods = MODS;
        return 1;
    }
    return 0;
}

/* ---- init ---------------------------------------------------- */
#if defined(__x86_64__) || defined(__i386__)

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define ST_OUT_FULL 0x01
#define ST_IN_FULL 0x02
#define ST_AUX 0x20

static int wait_write(void)
{
    for (int i = 0; i < 200000; i++)
        if (!(io_in8(PS2_STATUS) & ST_IN_FULL))
            return 0;
    return -1;
}

/* wait for a byte from the keyboard; mouse bytes that show up meanwhile are dropped */
static int read_kbd_byte(void)
{
    for (int i = 0; i < 400000; i++)
    {
        unsigned char st = io_in8(PS2_STATUS);
        if (st & ST_OUT_FULL)
        {
            unsigned char b = io_in8(PS2_DATA);
            if (!(st & ST_AUX))
                return b;
        }
    }
    return -1;
}

/* send a byte to the keyboard; 0 = it answered ACK (0xFA) */
static int kbd_cmd(unsigned char b)
{
    for (int tries = 0; tries < 3; tries++)
    {
        if (wait_write())
            return -1;
        io_out8(PS2_DATA, b);
        int r = read_kbd_byte();
        if (r == 0xFA)
            return 0;
        if (r != 0xFE) /* 0xFE = "send it again" */
            return -1;
    }
    return -1;
}

int keyboard_init(void)
{
    if (io_in8(PS2_STATUS) == 0xFF) /* floating bus: no controller */
        return -1;
    for (int i = 0; i < 64 && (io_in8(PS2_STATUS) & ST_OUT_FULL); i++)
        io_in8(PS2_DATA); /* drop old bytes */

    if (wait_write())
        return -1;
    io_out8(PS2_STATUS, 0xAE); /* enable the keyboard port */

    /* configuration byte: keyboard clock on (bit 4 = 0) and scan code translation on (bit 6 = 1),
     * so every keyboard delivers "set 1" codes, which is what the decoder above expects */
    if (wait_write())
        return -1;
    io_out8(PS2_STATUS, 0x20);
    for (int i = 0; i < 200000 && !(io_in8(PS2_STATUS) & ST_OUT_FULL); i++) {}
    if (!(io_in8(PS2_STATUS) & ST_OUT_FULL))
        return -1;
    unsigned char cfg = io_in8(PS2_DATA);
    cfg &= (unsigned char)~0x10;
    cfg |= 0x40;
    if (wait_write())
        return -1;
    io_out8(PS2_STATUS, 0x60);
    if (wait_write())
        return -1;
    io_out8(PS2_DATA, cfg);

    /* keyboard: scan code set 2 (the controller turns it into set 1), then start sending */
    int ok = !kbd_cmd(0xF0) && !kbd_cmd(0x02);
    ok = !kbd_cmd(0xF4) && ok;
    for (int i = 0; i < 64 && (io_in8(PS2_STATUS) & ST_OUT_FULL); i++)
        io_in8(PS2_DATA);
    return ok ? 0 : -1;
}

#else

int keyboard_init(void) { return -1; }

#endif
