/* HarisOS desktop */
#include "desktop.h"

static unsigned int *FB;
static unsigned int W, H, PITCH;
static unsigned int RED_MASK_MODE;
static const char *STATUS_LINE;

void desktop_set_status(const char *line) { STATUS_LINE = line; }

void desktop_set_framebuffer(void *framebuffer, unsigned int width,
                             unsigned int height, unsigned int pixels_per_scanline,
                             unsigned int red_mask_mode)
{
    FB = (unsigned int *)framebuffer;
    W = width;
    H = height;
    PITCH = pixels_per_scanline;
    RED_MASK_MODE = red_mask_mode;
}

static unsigned int rgb(unsigned char r, unsigned char g, unsigned char b)
{
    return RED_MASK_MODE ? ((unsigned int)r << 16) | ((unsigned int)g << 8) | b
                         : ((unsigned int)b << 16) | ((unsigned int)g << 8) | r;
}
static void pixel(int x, int y, unsigned int c)
{
    if (FB && x >= 0 && y >= 0 && (unsigned int)x < W && (unsigned int)y < H)
        FB[(unsigned long long)y * PITCH + (unsigned long long)x] = c;
}
static void rect(int x, int y, int w, int h, unsigned int c)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x<0 ? 0 : x, y0 = y<0 ? 0 : y, x1 = x + w>(int) W ? (int)W : x + w, y1 = y + h>(int) H ? (int)H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            pixel(xx, yy, c);
}

static void gradient(void)
{
    for (unsigned int y = 0; y < H; y++)
    {
        unsigned char r = (unsigned char)(12 + (28ULL * y / (H ? H : 1)));
        unsigned char g = (unsigned char)(45 + (58ULL * y / (H ? H : 1)));
        unsigned char b = (unsigned char)(86 + (76ULL * y / (H ? H : 1)));
        rect(0, (int)y, (int)W, 1, rgb(r, g, b));
    }
}
/* Compact 5x7 font for desktop labels. */
static const unsigned char *glyph(char c)
{
    static const unsigned char blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const unsigned char font[][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 18, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4}, {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17}, {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}, {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30}, {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30}, {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return font[c - 'A'];
    if (c >= '0' && c <= '9')
        return font[26 + c - '0'];
    return blank;
}
static void text(int x, int y, const char *s, unsigned int color, int scale)
{
    for (; *s; s++, x += 6 * scale)
    {
        const unsigned char *g = glyph(*s);
        for (int yy = 0; yy < 7; yy++)
            for (int xx = 0; xx < 5; xx++)
                if (g[yy] & (1 << (4 - xx)))
                    rect(x + xx * scale, y + yy * scale, scale, scale, color);
    }
}

static void desktop(void)
{
    unsigned int white = rgb(245, 249, 255), panel = rgb(20, 30, 48), blue = rgb(44, 120, 230);
    gradient();
    /* soft abstract wallpaper waves */
    rect((int)(W * 56 / 100), (int)(H * 16 / 100), (int)(W * 34 / 100), (int)(H * 48 / 100), rgb(29, 104, 169));
    rect((int)(W * 62 / 100), (int)(H * 22 / 100), (int)(W * 25 / 100), (int)(H * 36 / 100), rgb(34, 126, 185));
    rect((int)(W * 68 / 100), (int)(H * 28 / 100), (int)(W * 15 / 100), (int)(H * 24 / 100), rgb(42, 148, 199));

    /* a sample welcome window */
    int ww = (int)(W * 48 / 100), wh = (int)(H * 43 / 100), wx = ((int)W - ww) / 2, wy = ((int)H - wh) / 2 - 8;
    if (ww < 360)
        ww = 360;
    if (wh < 220)
        wh = 220;
    rect(wx + 7, wy + 8, ww, wh, rgb(9, 24, 42));
    rect(wx, wy, ww, wh, rgb(239, 244, 250));
    rect(wx, wy, ww, 34, blue);
    text(wx + 14, wy + 12, "HARISOS DESKTOP", white, 1);
    rect(wx + ww - 26, wy + 8, 16, 16, rgb(221, 75, 75));
    text(wx + 24, wy + 62, "WELCOME TO HARISOS", rgb(30, 54, 80), 2);
    text(wx + 24, wy + 104, "UEFI GRAPHICS DESKTOP", rgb(48, 75, 104), 1);
    rect(wx + 24, wy + 134, ww - 48, 1, rgb(190, 202, 216));
    text(wx + 24, wy + 151, "YOUR DESKTOP IS READY", rgb(36, 113, 90), 1);
    if (STATUS_LINE)
        text(wx + 24, wy + 171, STATUS_LINE, rgb(48, 75, 104), 1);
    rect(wx + 24, wy + wh - 48, 118, 28, blue);
    text(wx + 39, wy + wh - 38, "START", white, 1);
    /* bottom taskbar */
    rect(0, (int)H - 42, (int)W, 42, panel);
    rect(0, (int)H - 42, (int)W, 1, rgb(82, 111, 145));
    rect(10, (int)H - 34, 28, 26, blue);
    rect(16, (int)H - 28, 6, 6, white);
    rect(25, (int)H - 28, 6, 6, white);
    rect(16, (int)H - 19, 6, 6, white);
    rect(25, (int)H - 19, 6, 6, white);
    text(49, (int)H - 27, "HARISOS", white, 1);
    /* status tray */
    rect((int)W - 55, (int)H - 29, 8, 8, rgb(81, 211, 137));
    text((int)W - 40, (int)H - 26, "OK", white, 1);
}

void desktop_draw(void) { desktop(); }
