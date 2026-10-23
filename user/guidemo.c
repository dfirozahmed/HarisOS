/* HarisOS user program: a window of its own, drawn by the program (the compositor protocol of include/gui.h).
 *
 * It asks for a 320 x 200 surface (SYS_GUI_CREATE), draws a gradient and a square into the pixels the kernel mapped for it, and
 * tells the compositor which part changed (SYS_GUI_PRESENT). Then it waits for events (SYS_GUI_EVENT): the square follows the mouse
 * and changes colour with the buttons and with every key, the wheel resizes it. Escape, or the window's close button (which arrives
 * as an event - the program decides), ends it. Only the rectangle that changed is redrawn and presented. */
#include "sys.h"

#define W 320
#define H 200

#define KEY_ESC 256

static unsigned *PIX;

/* the background: dark blue at the top, to a lighter one at the bottom */
static unsigned bg(int y)
{
    unsigned t = (unsigned)y * 255 / (H - 1);
    return (20 + t / 8) << 16 | (30 + t / 5) << 8 | (60 + t / 3);
}

static void fill_bg(int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < W && yy >= 0 && yy < H)
                PIX[yy * W + xx] = bg(yy);
}

static void fill_box(int x, int y, int size, unsigned color)
{
    for (int yy = y; yy < y + size; yy++)
        for (int xx = x; xx < x + size; xx++)
            if (xx >= 0 && xx < W && yy >= 0 && yy < H)
                PIX[yy * W + xx] = (xx == x || yy == y || xx == x + size - 1 || yy == y + size - 1) ? 0xFFFFFF : color;
}

static const unsigned PALETTE[6] = {0xE0556B, 0xF0A030, 0xE8D84A, 0x50C878, 0x4FA0E8, 0xA070E0};

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    GuiInfo gi;
    for (usize i = 0; i < sizeof(gi); i++)
        ((char *)&gi)[i] = 0;
    const char *title = "GUI demo";
    for (int i = 0; title[i]; i++)
        gi.title[i] = title[i];
    gi.width = W;
    gi.height = H;
    if (sys_gui_create(&gi) != 0)
    {
        print("guidemo: no window\n");
        sys_exit(1);
    }
    PIX = (unsigned *)gi.pixels;
    int h = gi.handle;

    int size = 28, bx = (W - size) / 2, by = (H - size) / 2, color = 0, shown = 1;
    fill_bg(0, 0, W, H);
    fill_box(bx, by, size, PALETTE[color]);
    sys_gui_present(h, 0, 0, 0, 0);

    int done = 0;
    while (!done)
    {
        GuiEvent ev;
        if (sys_gui_event(h, &ev, 0) != 1)
            break; /* the window is gone (the compositor closed it, or a signal) */
        int nx = bx, ny = by, nsize = size, ncolor = color;
        switch (ev.type)
        {
        case GUI_EV_MOUSE_MOVE:
        case GUI_EV_MOUSE_DOWN:
            nx = ev.x - size / 2;
            ny = ev.y - size / 2;
            if (ev.type == GUI_EV_MOUSE_DOWN)
                ncolor = ev.buttons & 2 ? (color + 5) % 6 : (color + 1) % 6;
            break;
        case GUI_EV_KEY:
            if (!ev.pressed)
                break;
            if (ev.key == KEY_ESC)
                done = 1;
            else
                ncolor = (color + 1) % 6;
            break;
        case GUI_EV_WHEEL:
            nsize = size - ev.x * 4; /* wheel steps: + = down = smaller */
            if (nsize < 8)
                nsize = 8;
            if (nsize > 96)
                nsize = 96;
            break;
        case GUI_EV_CLOSE:
            done = 1;
            break;
        default:
            break;
        }
        if (done)
            break;
        if (nx == bx && ny == by && nsize == size && ncolor == color)
            continue;
        /* erase the old square, draw the new one, present both places as one rectangle */
        int x0 = bx < nx ? bx : nx, y0 = by < ny ? by : ny;
        int x1 = (bx + size > nx + nsize ? bx + size : nx + nsize), y1 = (by + size > ny + nsize ? by + size : ny + nsize);
        fill_bg(bx, by, size, size);
        bx = nx;
        by = ny;
        size = nsize;
        color = ncolor;
        fill_box(bx, by, size, PALETTE[color]);
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > W)
            x1 = W;
        if (y1 > H)
            y1 = H;
        if (x1 > x0 && y1 > y0)
            sys_gui_present(h, x0, y0, x1 - x0, y1 - y0);
        shown++;
    }
    sys_gui_destroy(h);
    sys_shm_unmap(PIX);
    print("guidemo: bye, ");
    print_number(shown);
    print(" pictures\n");
    sys_exit(0);
}
