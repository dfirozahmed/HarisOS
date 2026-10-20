/* PS/2 mouse through the i8042 keyboard controller (ports 0x60 / 0x64).
 * Everything is polled: the kernel runs with interrupts off. */
#include "mouse.h"
#include "ps2.h"
#include "usb.h"
#include "i2c_hid.h"

#if defined(__x86_64__) || defined(__i386__)

static void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define PS2_DATA 0x60
#define PS2_STATUS 0x64 /* read: status, write: command */
#define ST_OUT_FULL 0x01 /* a byte is waiting at 0x60 */
#define ST_IN_FULL 0x02  /* controller still busy with our last byte */
#define ST_AUX 0x20      /* the waiting byte comes from the mouse */

static int SW, SH;
static int PRESENT; /* a PS/2 mouse answered */
static int USB_OK;  /* an xHCI controller is running (a USB mouse may come and go) */
static int I2C_OK;  /* an I2C-HID touchpad is running */
static int HAS_WHEEL;
static int X, Y;
static unsigned BUTTONS, PS2_BUTTONS;
static unsigned char PKT[4];
static int PKT_LEN, PKT_NEED = 3;

static int wait_write(void) /* wait until we may send a byte */
{
    for (int i = 0; i < 200000; i++)
        if (!(inb(PS2_STATUS) & ST_IN_FULL))
            return 0;
    return -1;
}

static int wait_read(int tries) /* wait until a byte can be read */
{
    for (int i = 0; i < tries; i++)
        if (inb(PS2_STATUS) & ST_OUT_FULL)
            return 0;
    return -1;
}

static void flush(void)
{
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUT_FULL); i++)
        inb(PS2_DATA);
}

/* Send one byte to the mouse (not the keyboard). Returns the mouse's answer or -1. */
static int mouse_write(unsigned char b)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (wait_write())
            return -1;
        outb(PS2_STATUS, 0xD4); /* "next byte goes to the mouse" */
        if (wait_write())
            return -1;
        outb(PS2_DATA, b);
        if (wait_read(400000))
            return -1;
        int r = inb(PS2_DATA);
        if (r != 0xFE) /* 0xFE = resend; real touchpads do this */
            return r;
    }
    return -1;
}

static int mouse_cmd(unsigned char b) { return mouse_write(b) == 0xFA ? 0 : -1; } /* 0xFA = ACK */

static int mouse_cmd_arg(unsigned char b, unsigned char arg)
{
    if (mouse_cmd(b))
        return -1;
    return mouse_cmd(arg);
}

static int ps2_mouse_init(void)
{
    if (inb(PS2_STATUS) == 0xFF) /* floating bus: no controller at all */
        return -1;

    flush();
    if (wait_write())
        return -1;
    outb(PS2_STATUS, 0xA8); /* enable the mouse port */

    /* controller configuration byte: make sure the mouse clock is running */
    if (wait_write())
        return -1;
    outb(PS2_STATUS, 0x20);
    if (wait_read(200000))
        return -1;
    unsigned char cfg = inb(PS2_DATA);
    cfg &= (unsigned char)~0x20; /* bit 5 = 0: mouse clock enabled */
    if (wait_write())
        return -1;
    outb(PS2_STATUS, 0x60);
    if (wait_write())
        return -1;
    outb(PS2_DATA, cfg);

    /* reset: the mouse answers ACK, 0xAA (self test passed), device id */
    mouse_write(0xF5); /* stop any streaming left over from the firmware */
    flush();
    int r = -1;
    for (int attempt = 0; attempt < 3 && r != 0xFA; attempt++)
    {
        r = mouse_write(0xFF);
        if (r == 0xFA)
        {
            if (!wait_read(3000000))
                inb(PS2_DATA); /* 0xAA */
            if (!wait_read(3000000))
                inb(PS2_DATA); /* id */
        }
        flush();
    }
    if (r != 0xFA && mouse_write(0xF6) != 0xFA)
        return -1; /* nothing connected: neither reset nor defaults answered */

    mouse_cmd(0xF6); /* defaults */

    /* Magic sample-rate sequence 200,100,80 switches on the scroll wheel; the id becomes 3. */
    if (!mouse_cmd_arg(0xF3, 200) && !mouse_cmd_arg(0xF3, 100) && !mouse_cmd_arg(0xF3, 80))
    {
        if (!mouse_cmd(0xF2) && !wait_read(200000))
        {
            unsigned char id = inb(PS2_DATA);
            if (id == 3 || id == 4)
            {
                HAS_WHEEL = 1;
                PKT_NEED = 4;
            }
        }
    }
    mouse_cmd_arg(0xF3, 100); /* 100 reports per second */
    mouse_cmd_arg(0xE8, 3);   /* 8 counts per mm: finer movement */
    if (mouse_cmd(0xF4))      /* start sending movement */
        return -1;
    flush();
    PRESENT = 1;
    return 0;
}

int mouse_init(int w, int h)
{
    SW = w;
    SH = h;
    X = w / 2;
    Y = h / 2;
    BUTTONS = PS2_BUTTONS = 0;
    PKT_LEN = 0;
    PRESENT = 0;
    USB_OK = I2C_OK = 0;
    HAS_WHEEL = 0;
    PKT_NEED = 3;

    ps2_mouse_init();
    USB_OK = usb_init() == 0;
    I2C_OK = i2c_hid_init() == 0;
    return (PRESENT || USB_OK || I2C_OK) ? 0 : -1;
}

int mouse_present(void) { return PRESENT || USB_OK || I2C_OK; }

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Turn pointer speed into a gentle acceleration: slow moves stay precise. */
static int accel(int d)
{
    int a = d < 0 ? -d : d;
    int r = a <= 4 ? a : (a <= 12 ? a + (a - 4) / 2 : a + 4 + (a - 12));
    return d < 0 ? -r : r;
}

int mouse_poll(MouseState *out)
{
    int changed = 0, dx = 0, dy = 0, wheel = 0;
    unsigned old = BUTTONS;

    if (PRESENT)
    {
        ps2_poll(); /* sorts keyboard and mouse bytes into their own queues */
        unsigned char b;
        for (int n = 0; n < 64 && ps2_mouse_pop(&b); n++) /* never spend long here */
        {
            /* byte 0 of every packet has bit 3 set: use it to re-synchronize */
            if (PKT_LEN == 0 && !(b & 0x08))
                continue;
            PKT[PKT_LEN++] = b;
            if (PKT_LEN < PKT_NEED)
                continue;
            PKT_LEN = 0;

            if (PKT[0] & 0xC0)
                continue; /* overflow: the movement values are garbage */
            int mx = PKT[1] - ((PKT[0] & 0x10) ? 256 : 0);
            int my = PKT[2] - ((PKT[0] & 0x20) ? 256 : 0);
            dx += mx;
            dy -= my; /* the mouse reports "up" as positive */
            if (HAS_WHEEL)
            {
                int z = PKT[3] & 0x0F;
                wheel += z >= 8 ? z - 16 : z;
            }
            PS2_BUTTONS = PKT[0] & 7;
        }
    }

    unsigned usb_buttons = 0;
    if (USB_OK)
        usb_poll(&dx, &dy, &wheel, &usb_buttons);
    unsigned i2c_buttons = 0;
    if (I2C_OK)
        i2c_hid_poll(&dx, &dy, &wheel, &i2c_buttons);
    BUTTONS = PS2_BUTTONS | usb_buttons | i2c_buttons;

    if (dx || dy)
    {
        X = clamp(X + accel(dx), 0, SW - 1);
        Y = clamp(Y + accel(dy), 0, SH - 1);
        changed = 1;
    }
    if (wheel || BUTTONS != old)
        changed = 1;

    out->x = X;
    out->y = Y;
    out->dx = dx;
    out->dy = dy;
    out->wheel = wheel;
    out->buttons = BUTTONS;
    return changed;
}

#else /* not x86: no PS/2 controller */

int mouse_init(int w, int h) { (void)w; (void)h; return -1; }
int mouse_present(void) { return 0; }
int mouse_poll(MouseState *out)
{
    out->x = out->y = out->dx = out->dy = out->wheel = 0;
    out->buttons = 0;
    return 0;
}

#endif
